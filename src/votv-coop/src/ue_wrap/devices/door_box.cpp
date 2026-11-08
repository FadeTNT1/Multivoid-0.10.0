// ue_wrap/devices/door_box.cpp -- see ue_wrap/devices/door_box.h.

#include "ue_wrap/devices/door_box.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/drone_console.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"

#include <chrono>
#include <cmath>
#include <cwchar>
#include <cstdint>
#include <vector>

namespace ue_wrap::door_box {
namespace {

namespace R = reflection;

// Per-class resolution. The wrapper is operational when EITHER class resolved
// (the console may stream in later than the lockers or vice versa). The class is kept only while its
// slot and serial still hold it -- a Blueprint class can get a new object in a new world -- and the
// family's verbs (its apply verb, a__UpdateFunc which rotates the axis from alpha, a__FinishedFunc
// which on a locker slams it shut and restores the collision) resolve per instance.
struct ClassDesc {
    const wchar_t* name = nullptr;  // the family's class
    const wchar_t* verb = nullptr;  // its own apply verb
    ue_wrap::CachedObjRef clsRef;   // the class as this world holds it
    bool    resolved = false;       // the offsets below are read
    void*   failedCls = nullptr;  // a class whose `opened` did not resolve: said once, not asked again
    int32_t offOpened = -1;
    int32_t offAlpha = -1;      // the swing timeline's value, a_a_<guid>, by its prefix
    int32_t offDirection = -1;  // its TEnumAsByte<ETimelineDirection>, a__Direction_<guid>
    int32_t offTimeline = -1;   // UTimelineComponent* A
    int32_t offTrigger = -1;    // locker only: AActor* triggerOnOpen (the davyJones gate)
};

ClassDesc g_locker;   // Alocker_C (+ subclasses locker_personal_C / locker_death_C); verb = Open(bool)
ClassDesc g_console;  // AdroneConsole_C; verb = setButtonsCollision()
void* g_timelinePlayFn = nullptr;       // UTimelineComponent::Play
void* g_timelineReverseFn = nullptr;    // UTimelineComponent::Reverse
bool  g_timelineFailed = false;         // the engine class held no Play or Reverse: said once

// Force-snap verify queue: the 0.5 s swing Timeline only advances while the actor TICKS, so far
// from the local player it freezes and the door leaf sticks mid-swing (door.cpp drives the same
// swing and carries the same note). Entries clear on completion or snap.
struct Verify {
    void* actor;
    int32_t idx;
    bool want;
    bool isLocker;
    std::chrono::steady_clock::time_point deadline;
};
std::vector<Verify> g_verify;  // GT-only

// The family's class as this world holds it, looked up by name again once its slot or serial no longer
// holds the kept one. Game thread.
void* ClassFor(ClassDesc& d) {
    if (void* c = d.clsRef.Get()) return c;
    void* c = d.name ? ue_wrap::object_index::ClassByName(d.name) : nullptr;
    if (c) d.clsRef.Set(c);
    return c;
}

// One of an instance's verbs: its class's own, memoised by the reflection layer.
void* Verb(void* actor, const wchar_t* name) {
    void* cls = actor ? R::ClassOf(actor) : nullptr;
    return cls && name ? R::FindDispatchFunctionCached(cls, name) : nullptr;
}

// A family's offsets are read on the ONE tick its class first appears, each family on its own edge: a
// latch shared with the family that streamed in first would have left the second unresolved. Every
// offset is read by name, the swing timeline's GUID-suffixed ones by their prefix; a family whose
// `opened` does not resolve stays out, said once, and one whose swing does not runs without the snap.
bool ResolveClass(ClassDesc& d, const wchar_t* clsName, const wchar_t* verbName) {
    d.name = clsName;
    d.verb = verbName;
    if (d.resolved) return true;
    void* cls = ClassFor(d);
    if (!cls || cls == d.failedCls) return false;
    const int32_t opened = R::FindPropertyOffset(cls, L"opened");
    if (opened < 0) {
        d.failedCls = cls;
        UE_LOGE("door_box: %ls.opened did not resolve by name -- that family stays out of the sync", clsName);
        return false;
    }
    d.offOpened = opened;
    d.offAlpha = R::FindPropertyOffsetByPrefix(cls, L"a_a_");
    d.offDirection = R::FindPropertyOffsetByPrefix(cls, L"a__Direction_");
    d.offTimeline = R::FindPropertyOffset(cls, L"a");
    d.offTrigger = R::FindPropertyOffset(cls, L"triggerOnOpen");  // locker only; -1 on the console
    d.resolved = true;
    // The verbs are asked here only so a build without them says so; each call resolves its own.
    void* const verbFn = R::FindFunction(cls, verbName);
    void* const updateFn = R::FindFunction(cls, L"a__UpdateFunc");
    void* const finishedFn = R::FindFunction(cls, L"a__FinishedFunc");
    if (!verbFn)
        UE_LOGW("door_box: %ls::%ls unresolved -- that family's apply degrades to snap-only",
                clsName, verbName);
    if (d.offAlpha < 0 || d.offDirection < 0 || d.offTimeline < 0)
        UE_LOGW("door_box: %ls's swing timeline did not resolve (value %d, direction %d, component %d) -- no "
                "snap for that family", clsName, d.offAlpha, d.offDirection, d.offTimeline);
    UE_LOGI("door_box: resolved %ls=%p opened@0x%04X swing@%d/%d/%d verb=%p upd=%p fin=%p", clsName, cls,
            opened, d.offAlpha, d.offDirection, d.offTimeline, verbFn, updateFn, finishedFn);
    return true;
}

// Whether the swing of `d`'s class can be read and snapped.
bool SwingReadable(const ClassDesc& d) { return d.offAlpha >= 0 && d.offDirection >= 0; }

inline bool ReadBool(const void* obj, int32_t off) {
    return *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(obj) + off);
}

// Which family is `obj`? (nullptr ClassDesc when neither.)
const ClassDesc* DescOf(void* obj) {
    if (!obj) return nullptr;
    void* cls = R::ClassOf(obj);
    if (!cls) return nullptr;
    for (ClassDesc* d : { &g_locker, &g_console }) {
        if (!d->resolved) continue;
        void* base = ClassFor(*d);
        if (base && R::IsDescendantOfAny(cls, &base, 1)) return d;
    }
    return nullptr;
}

// Snap the swing to its end state: write the timeline track float + direction, then dispatch
// a__UpdateFunc, which rotates the axis and is the VISUAL move -- FinishedFunc alone does not move
// the mesh -- and a__FinishedFunc, which on a locker close plays the slam and restores the door
// collision, so it is required.
void ForceSnap(void* actor, const ClassDesc& d, bool want) {
    if (!SwingReadable(d)) return;
    *reinterpret_cast<float*>(reinterpret_cast<char*>(actor) + d.offAlpha) = want ? 1.0f : 0.0f;
    *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(actor) + d.offDirection) =
        want ? 0u : 1u;  // ETimelineDirection: 0=Forward, 1=Backward
    if (void* fn = Verb(actor, L"a__UpdateFunc")) { ParamFrame f(fn); if (f.valid()) Call(actor, f); }
    if (void* fn = Verb(actor, L"a__FinishedFunc")) { ParamFrame f(fn); if (f.valid()) Call(actor, f); }
}

// The timeline verbs belong to no family, so they are resolved on their own rather than with
// whichever class latched first. The engine's class is always loaded; a verb it does not hold is
// said once and not asked for again.
void ResolveTimelineVerbs() {
    if (g_timelinePlayFn || g_timelineFailed) return;
    void* tc = ue_wrap::object_index::ClassByName(L"TimelineComponent");
    if (!tc) return;
    g_timelinePlayFn    = R::FindFunction(tc, L"Play");
    g_timelineReverseFn = R::FindFunction(tc, L"Reverse");
    if (!g_timelinePlayFn || !g_timelineReverseFn) {
        g_timelinePlayFn = g_timelineReverseFn = nullptr;
        g_timelineFailed = true;
        UE_LOGW("door_box: UTimelineComponent Play/Reverse unresolved -- swings snap instead of animating");
    }
}

}  // namespace

bool EnsureResolved() {
    // A locker class that never appears is told once, so a renamed class tells itself: the clock starts at the
    // first ask (the lane's first tick), and a main-menu session reads as the same silence.
    static const auto s_start = std::chrono::steady_clock::now();
    static bool s_saidMissing = false;
    if (!s_saidMissing && !g_locker.resolved &&
        std::chrono::steady_clock::now() - s_start > std::chrono::seconds(90) && !ClassFor(g_locker)) {
        s_saidMissing = true;
        UE_LOGW("door_box: class 'locker_C' not found in the object index 90 s after the lane started -- the lockers "
                "cannot sync (renamed class on this build, or no world has loaded; ignore in the main menu)");
    }
    // Both families are tried until each has its class: whichever streams in second resolves its
    // own verb on its own edge, which a latch shared with the first would have denied it forever.
    // A class held costs one pointer test, and one not loaded one object-index lookup, which costs
    // nothing on a miss.
    ResolveClass(g_locker, L"locker_C", L"Open");
    ResolveClass(g_console, L"droneConsole_C", L"setButtonsCollision");
    ResolveTimelineVerbs();
    return g_locker.resolved || g_console.resolved;
}

uint64_t ResolvedFamilyCount() { return (g_locker.resolved ? 1u : 0u) + (g_console.resolved ? 1u : 0u); }

bool LockerClassResolved() { return g_locker.resolved; }

bool IsDoorBox(void* obj) { return DescOf(obj) != nullptr; }
bool IsLocker(void* obj) { return obj && g_locker.resolved && DescOf(obj) == &g_locker; }
bool IsDroneConsole(void* obj) { return obj && g_console.resolved && DescOf(obj) == &g_console; }

bool CallAction(void* actor, void* player, uint8_t action) {
    void* cls = actor ? R::ClassOf(actor) : nullptr;
    void* fn = cls ? R::FindDispatchFunctionCached(cls, L"actionOptionIndex") : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid() || !f.Set<void*>(L"player", player) || !f.Set<uint8_t>(L"action", action)) return false;
    return Call(actor, f);
}

std::wstring GetNameKey(void* actor) {
    if (!actor) return std::wstring();
    return R::ToString(R::NameOf(actor));
}

std::wstring GetPlacementIdentity(void* actor) {
    const ClassDesc* d = DescOf(actor);
    if (!d) return std::wstring();
    FVector loc;
    if (!ue_wrap::engine::TryGetActorLocation(actor, loc)) return std::wstring();
    wchar_t buf[96];
    swprintf(buf, 96, L"%ls@%ld,%ld,%ld", d == &g_locker ? L"locker" : L"console",
             std::lround(loc.X), std::lround(loc.Y), std::lround(loc.Z));
    return std::wstring(buf);
}

bool TryReadOpened(void* actor, bool& out) {
    const ClassDesc* d = DescOf(actor);
    if (!d || d->offOpened < 0) return false;
    out = ReadBool(actor, d->offOpened);
    return true;
}

bool ApplyOpened(void* actor, bool want) {
    const ClassDesc* d = DescOf(actor);
    if (!d) return false;
    const bool isLocker = (d == &g_locker);
    // Trigger-wired locker (locker_davyJones: triggerOnOpen -> a batchSpawner): the native Open
    // verb FIRES the trigger -- correct on the OPENER, where it runs once, and wrong on a MIRROR
    // apply, where the spawner would run a second time on this peer and double-spawn. Targeted
    // per-site fix (principle 4): a trigger-wired locker mirrors via the SNAP path (opened write +
    // ForceSnap swing), so door state mirrors and the trigger fires only where the player actually
    // opened it. The test is per instance, on this locker's own triggerOnOpen: a locker whose
    // trigger is null keeps the full native verb.
    const bool triggerWired =
        isLocker && d->offTrigger >= 0 &&
        *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(actor) + d->offTrigger) != nullptr;
    void* const verb = Verb(actor, d->verb);
    if (isLocker && verb && !triggerWired) {
        // The full native verb: writes opened, plays the sound, swings the
        // Timeline, sets door collision.
        ParamFrame f(verb);
        if (!f.valid()) return false;
        static bool s_saidParam = false, s_saidCall = false;
        if (!f.Set<bool>(L"opened", want) && !s_saidParam) {
            s_saidParam = true;
            UE_LOGW("door_box: locker open() has no parameter named 'opened' on this build -- the apply calls it "
                    "with the default, so a peer's state will not land");
        }
        if (!Call(actor, f)) {
            if (!s_saidCall) {
                s_saidCall = true;
                UE_LOGW("door_box: the locker's open(%d) call failed on '%ls'", want ? 1 : 0, GetNameKey(actor).c_str());
            }
            return false;
        }
    } else if (triggerWired) {
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(actor) + d->offOpened) = want;
        ForceSnap(actor, *d, want);
        UE_LOGI("door_box: trigger-wired locker mirrored via snap (want=%d) -- "
                "triggerOnOpen left to the opening peer", want ? 1 : 0);
        return true;  // snapped synchronously -- no verify entry needed
    } else {
        static bool s_saidNoVerb = false;
        if (isLocker && !s_saidNoVerb) {
            s_saidNoVerb = true;
            UE_LOGW("door_box: the locker has no open() verb on this build -- its apply falls to the console path "
                    "(write opened, drive the timeline), which skips the sound and the collision");
        }
        // Console (no public verb) -- the garage write+refresh precedent:
        // opened := want, refresh the button/blinklight collision, drive the
        // swing natively via the Timeline component.
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(actor) + d->offOpened) = want;
        if (d == &g_console && verb) {
            ParamFrame f(verb);
            if (f.valid()) Call(actor, f);
        }
        // The console's own press also plays the lid's `audio_door` (the bytecode of actionOptionIndex, action
        // 10/11, right after setButtonsCollision), so a peer that only mirrors the lid hears it open and shut
        // like the one that pressed it.
        if (d == &g_console) {
            void* const doorCue = ue_wrap::drone_console::PartOf(actor, L"audio_door");
            if (doorCue) ue_wrap::engine::PlayAudioComponent(doorCue);
        }
        void* tl = d->offTimeline < 0 ? nullptr : *reinterpret_cast<void* const*>(
            reinterpret_cast<const char*>(actor) + d->offTimeline);
        void* fn = want ? g_timelinePlayFn : g_timelineReverseFn;
        if (tl && R::IsLive(tl) && fn) {
            ParamFrame f(fn);
            if (f.valid()) Call(tl, f);
        }
    }
    // Verify + force-snap for the far-frozen-timeline case, where the swing can be read.
    if (SwingReadable(*d))
        g_verify.push_back(Verify{ actor, R::InternalIndexOf(actor), want, isLocker,
                                   std::chrono::steady_clock::now() + std::chrono::milliseconds(1500) });
    return true;
}

void OnDisconnect() {
    // A swing mid-verify at session teardown must not survive into the next session or an SP world:
    // a stale entry's ForceSnap (a__UpdateFunc / a__FinishedFunc) would fire on a world state it
    // was not queued for.
    g_verify.clear();
}

void TickVerify() {
    if (g_verify.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_verify.begin(); it != g_verify.end();) {
        if (!R::IsLiveByIndex(it->actor, it->idx)) { it = g_verify.erase(it); continue; }
        const ClassDesc& d = it->isLocker ? g_locker : g_console;
        const float alpha = *reinterpret_cast<const float*>(
            reinterpret_cast<const char*>(it->actor) + d.offAlpha);
        const bool done = it->want ? (alpha >= 0.99f) : (alpha <= 0.01f);
        if (done) { it = g_verify.erase(it); continue; }
        if (now >= it->deadline) {
            ForceSnap(it->actor, d, it->want);
            UE_LOGI("door_box: force-snapped frozen swing (want=%d)", it->want ? 1 : 0);
            it = g_verify.erase(it);
            continue;
        }
        ++it;
    }
}

}  // namespace ue_wrap::door_box
