// ue_wrap/devices/garage.cpp -- see ue_wrap/devices/garage.h. Engine access for the base garage
// door (Agarage_C). Offsets and verbs are resolved from the live class by name; a class whose Open or
// acivae does not resolve leaves the lane off, said once, since a guessed offset would write whatever a
// newer build keeps there.

#include "ue_wrap/devices/garage.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"

#include <atomic>
#include <cmath>
#include <cstdint>

namespace ue_wrap::garage {
namespace {

namespace R = reflection;
namespace N = ue_wrap::profile::name;

std::atomic<bool> g_resolved{false};

int32_t g_openOff   = -1;       // Agarage_C::Open
int32_t g_movOff    = -1;       // Agarage_C::mov, true mid-swing (a drill reads it; NormalizeOpen clears it)
int32_t g_meshOff   = -1;       // Agarage_C::SkeletalMesh, the door mesh the swing montage plays on (optional: SeedTo)
int32_t g_loopOff   = -1;       // Agarage_C::loop, the swing sound (optional)
int32_t g_navOff    = -1;       // Agarage_C::navmodifier (optional)
int32_t g_moveTlOff = -1;       // Agarage_C::move, the swing UTimelineComponent* (optional: NormalizeOpen)
// The verbs, resolved per instance (Verb below): runTrigger(owner, index), the wall button's call (a drill's),
// and acivae() -- the NATIVE animated swing (the montage from position 0 at half rate, and the move timeline
// over its full length). NOT settime, which runs the same timeline and then JUMPS it: the montage at full
// rate from position 100, then move.SetNewTime to second 0 or 1 of a six-second track.
// No Key offset: identity is the level-export FName (GetNameKey), not the save key -- see
// garage.h for why the key cannot serve as one.

// A garage class whose Open or acivae did not resolve: said once, asked again only for another class
// object.
void* g_failedCls = nullptr;

// garage_C as this world holds it: kept only while its slot and serial still hold it, looked up by name again
// after a world gave the class a new object. Game thread.
ue_wrap::CachedObjRef g_garageClsRef;
void* GarageClass() {
    if (void* c = g_garageClsRef.Get()) return c;
    void* c = ue_wrap::object_index::ClassByName(L"garage_C");
    if (c) g_garageClsRef.Set(c);
    return c;
}

// One of the garage's verbs on this instance: its class's own, memoised by the reflection layer.
void* Verb(void* g, const wchar_t* name) {
    void* cls = g ? R::ClassOf(g) : nullptr;
    return cls ? R::FindDispatchFunctionCached(cls, name) : nullptr;
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    // One object-index lookup a call until the class loads, which costs nothing while it has not.
    void* cls = GarageClass();
    if (!cls || cls == g_failedCls) return false;
    const int32_t openOff = R::FindPropertyOffset(cls, L"Open");
    void* acivaeFn = R::FindFunction(cls, L"acivae");
    if (openOff < 0 || !acivaeFn) {
        g_failedCls = cls;
        UE_LOGE("garage: garage_C did not resolve by name (Open@%d acivae=%p) -- the garage lane stays off for "
                "this class", openOff, acivaeFn);
        return false;
    }

    g_openOff   = openOff;
    g_movOff    = R::FindPropertyOffset(cls, N::GarageMovingProp);
    g_moveTlOff = R::FindPropertyOffset(cls, N::GarageMoveTimelineProp);
    g_meshOff   = R::FindPropertyOffset(cls, N::GarageMeshProp);
    g_loopOff   = R::FindPropertyOffset(cls, N::GarageLoopSoundProp);
    g_navOff    = R::FindPropertyOffset(cls, N::GarageNavModifierProp);
    void* const runTriggerFn = R::FindFunction(cls, L"runTrigger");  // asked here only to say its absence
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("garage: resolved garage_C=%p Open@0x%04X acivae=%p (identity=level-export FName)",
            cls, openOff, acivaeFn);
    if (g_moveTlOff < 0)
        UE_LOGW("garage: the move timeline (%ls@%d) did not resolve -- a load or join cannot normalise a garage "
                "whose Open flag already matches", N::GarageMoveTimelineProp, g_moveTlOff);
    if (g_movOff < 0 || !runTriggerFn)
        UE_LOGW("garage: mov@%d runTrigger=%p did not both resolve -- a drill cannot wait for a garage's rest "
                "or toggle it", g_movOff, runTriggerFn);
    return true;
}

bool IsGarage(void* obj) {
    if (!obj || !g_resolved.load(std::memory_order_acquire)) return false;
    void* garageCls = GarageClass();
    void* cls = R::ClassOf(obj);
    return garageCls && cls && R::IsDescendantOfAny(cls, &garageCls, 1);
}

std::wstring GetNameKey(void* g) {
    // Identity is the garage's level-export FName, baked into the cooked package and so
    // deterministic and cross-peer stable, NOT the save key. Mirrors ue_wrap::door_box::GetNameKey,
    // the proven author for keyless placed actors; garage.h says why the save key is unreliable.
    if (!g) return std::wstring();
    return R::ToString(R::NameOf(g));
}

bool TryReadOpen(void* g, bool& open) {
    if (!g || g_openOff < 0) return false;
    open = *reinterpret_cast<const bool*>(
        reinterpret_cast<const char*>(g) + g_openOff);
    return true;
}

bool ApplyOpen(void* g, bool open) {
    void* const acivae = Verb(g, L"acivae");
    if (!acivae) return false;
    // Idempotent: if already in the target state, do nothing (skip the re-trigger + the echo).
    bool cur = false;
    if (TryReadOpen(g, cur) && cur == open) return true;
    // Two facts about the blueprint drive this:
    //   (1) Neither settime() nor acivae() writes the `Open` bool. The only writers
    //       are runTrigger's E-press toggle and the game's own loadTriggerData
    //       (`open := value; settime`). So we must set the field ourselves, or the
    //       mirror's poll baseline goes stale and the symmetric Channel re-broadcasts
    //       the opposite -- an open/close oscillation.
    //   (2) settime() JUMPS: it plays the timeline, then puts the montage at position
    //       100 at full rate and calls move.SetNewTime with second 0 or 1 of a
    //       six-second track -- a close snaps shut, an open lurches and runs on.
    //       acivae() ANIMATES: the montage from 0 at half rate and move.Play/Reverse
    //       over the full timeline, direction read from the `Open` field.
    // So: write Open := target FIRST -- fixing the oscillation and giving acivae its direction --
    // THEN call acivae() for the native animated swing. acivae has no `mov` guard; that sits in
    // runTrigger, which ignores an E-press mid-swing, so a mid-swing opposite packet re-aims the
    // door, last writer wins. This is the local E-press path minus the toggle and that guard.
    if (g_openOff >= 0)
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(g) + g_openOff) = open;
    ParamFrame f(acivae);  // acivae() takes no params -- it reads the Open field for direction
    if (!f.valid()) return false;
    return Call(g, f);
}

namespace {

// The garage's move timeline, or null when it does not resolve or is not live.
void* MoveTimeline(void* g) {
    if (!g || g_moveTlOff < 0) return nullptr;
    void* tl = *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(g) + g_moveTlOff);
    return (tl && R::IsLive(tl)) ? tl : nullptr;
}

// A timeline verb on this component, by name; null when its class does not have it.
void* TimelineVerb(void* tl, const wchar_t* name) {
    void* cls = R::ClassOf(tl);
    return cls ? R::FindDispatchFunctionCached(cls, name) : nullptr;
}

bool TimelineFloat(void* tl, const wchar_t* verb, float& out) {
    void* fn = TimelineVerb(tl, verb);
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid() || !Call(tl, f)) return false;
    out = f.Get<float>(L"ReturnValue");
    return std::isfinite(out);
}

}  // namespace

Settled NormalizeOpen(void* g, bool open, float* before, float* after) {
    void* const tl = MoveTimeline(g);
    if (!tl) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGW("garage: NormalizeOpen has no move timeline to read (offset %d) -- a garage whose Open flag "
                    "already matches is left as it stands", g_moveTlOff);
        }
        return Settled::Unavailable;
    }
    void* const stopFn = TimelineVerb(tl, N::TimelineStopFn);
    void* const setFn = TimelineVerb(tl, N::TimelineSetPositionFn);
    void* const playingFn = TimelineVerb(tl, N::TimelineIsPlayingFn);
    float length = 0.f, pos = 0.f;
    if (!stopFn || !setFn || !playingFn || !TimelineFloat(tl, N::TimelineGetLengthFn, length) ||
        !TimelineFloat(tl, N::TimelineGetPositionFn, pos) || length <= 0.f) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGW("garage: the move timeline's verbs did not resolve or read (Stop=%p SetPlaybackPosition=%p "
                    "IsPlaying=%p length=%.2f) -- a garage whose Open flag already matches is left as it stands",
                    stopFn, setFn, playingFn, length);
        }
        return Settled::Unavailable;
    }
    if (before) *before = pos;
    if (after) *after = pos;
    bool moving = false;
    TryReadMoving(g, moving);
    ParamFrame playing(playingFn);
    const bool timelinePlaying = playing.valid() && Call(tl, playing) && playing.Get<bool>(L"ReturnValue");
    if (moving || timelinePlaying) return Settled::Busy;
    const float target = open ? length : 0.f;
    // The flag is written either way: a snapshot's state is the authority's, and a native load may have
    // left the other value.
    if (g_openOff >= 0) *reinterpret_cast<bool*>(reinterpret_cast<char*>(g) + g_openOff) = open;
    constexpr float kEps = 0.01f;
    if (std::fabs(pos - target) <= kEps) return Settled::Already;
    ParamFrame stop(stopFn);
    if (stop.valid()) Call(tl, stop);
    ParamFrame set(setFn);
    if (!set.valid() || !set.Set<float>(N::TimelineNewPositionParam, target) ||
        !set.Set<bool>(N::TimelineFireEventsParam, false) || !set.Set<bool>(N::TimelineFireUpdateParam, true) ||
        !Call(tl, set))
        return Settled::Unavailable;
    if (g_movOff >= 0) *reinterpret_cast<bool*>(reinterpret_cast<char*>(g) + g_movOff) = false;
    float now = target;
    TimelineFloat(tl, N::TimelineGetPositionFn, now);
    if (after) *after = now;
    return Settled::Snapped;
}

bool TryReadMoving(void* g, bool& moving) {
    if (!g || g_movOff < 0) return false;
    moving = *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(g) + g_movOff);
    return true;
}

bool CallRunTrigger(void* g, void* owner, int32_t index) {
    void* const fn = Verb(g, L"runTrigger");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid() || !f.Set<void*>(L"owner", owner) || !f.Set<int32_t>(L"index", index)) return false;
    return Call(g, f);
}

namespace {

namespace P = ue_wrap::profile;

constexpr uint32_t kSeedDriven = 1u;   // SeedTo has driven the door to its seed

void WriteOpen(void* g, bool open) {
    if (g_openOff >= 0) *reinterpret_cast<bool*>(reinterpret_cast<char*>(g) + g_openOff) = open;
}

// A component this garage holds at `off`, or null when the offset did not resolve or it is not live.
void* ComponentAt(void* g, int32_t off) {
    if (!g || off < 0) return nullptr;
    void* c = *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(g) + off);
    return (c && R::IsLive(c)) ? c : nullptr;
}

// The door mesh's live animation instance, or null while it has none (the mesh not initialised yet, or a world's
// first frames): the seed waits for it, since a montage played before it exists plays nowhere.
void* DoorAnimInstance(void* g) {
    void* const mesh = ComponentAt(g, g_meshOff);
    if (!mesh) return nullptr;
    void* const anim = *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(mesh) +
                                                       P::off::USkeletalMesh_AnimScriptInstance);
    return (anim && R::IsLive(anim)) ? anim : nullptr;
}

void* AnimVerb(void* anim, const wchar_t* name) {
    void* cls = R::ClassOf(anim);
    return cls ? R::FindDispatchFunctionCached(cls, name) : nullptr;
}

void SaidOnce(bool& said, const wchar_t* verb) {
    if (said) return;
    said = true;
    UE_LOGW("garage: UAnimInstance::%ls did not resolve -- the door mesh is left as the swing montage puts it", verb);
}

// The montage the door mesh is playing (the swing proxy's), or null.
void* ActiveMontage(void* anim) {
    static bool s_said = false;
    void* const fn = AnimVerb(anim, N::AnimGetActiveMontageFn);
    if (!fn) { SaidOnce(s_said, N::AnimGetActiveMontageFn); return nullptr; }
    ParamFrame f(fn);
    if (!f.valid() || !Call(anim, f)) return nullptr;
    void* const m = f.Get<void*>(L"ReturnValue");
    return (m && R::IsLive(m)) ? m : nullptr;
}

bool JumpToSectionsEnd(void* anim, void* montage, bool open) {
    static bool s_said = false;
    void* const fn = AnimVerb(anim, N::AnimJumpToSectionsEndFn);
    if (!fn) { SaidOnce(s_said, N::AnimJumpToSectionsEndFn); return false; }
    ParamFrame f(fn);
    R::FName section = ue_wrap::fname_utils::StringToFName(open ? N::GarageOpenSection : N::GarageCloseSection);
    return f.valid() && f.SetRaw(L"SectionName", &section, sizeof(section)) && f.Set<void*>(L"Montage", montage) &&
           Call(anim, f);
}

bool MontagePosition(void* anim, void* montage, float& pos) {
    static bool s_said = false;
    void* const fn = AnimVerb(anim, N::AnimMontageGetPositionFn);
    if (!fn) { SaidOnce(s_said, N::AnimMontageGetPositionFn); return false; }
    ParamFrame f(fn);
    if (!f.valid() || !f.Set<void*>(L"Montage", montage) || !Call(anim, f)) return false;
    pos = f.Get<float>(L"ReturnValue");
    return std::isfinite(pos);
}

bool SetMontagePosition(void* anim, void* montage, float pos) {
    static bool s_said = false;
    void* const fn = AnimVerb(anim, N::AnimMontageSetPositionFn);
    if (!fn) { SaidOnce(s_said, N::AnimMontageSetPositionFn); return false; }
    ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"Montage", montage) && f.Set<float>(L"NewPosition", pos) && Call(anim, f);
}

void SetComponentActive(void* comp, bool active) {
    void* cls = R::ClassOf(comp);
    void* const fn = cls ? R::FindDispatchFunctionCached(cls, N::ComponentSetActiveFn) : nullptr;
    if (!fn) return;
    ParamFrame f(fn);
    if (f.valid() && f.Set<bool>(L"bNewActive", active) && f.Set<bool>(L"bReset", false)) Call(comp, f);
}

// The blocking nav area the finished closing swing sets (the opening one clears it, and settime does the same).
void SetClosedNavArea(void* g) {
    void* const nav = ComponentAt(g, g_navOff);
    if (!nav) return;
    static void* s_areaCls = nullptr;
    // A Blueprint class can be collected with the world that loaded it: the cached pointer is
    // re-checked before it is handed to the engine, and looked up again when it went.
    if (s_areaCls && !R::IsLive(s_areaCls)) s_areaCls = nullptr;
    if (!s_areaCls) s_areaCls = R::FindClass(N::GarageClosedNavAreaClass);
    void* cls = R::ClassOf(nav);
    void* const fn = cls ? R::FindDispatchFunctionCached(cls, N::NavModifierSetAreaClassFn) : nullptr;
    if (!s_areaCls || !fn) return;
    ParamFrame f(fn);
    if (f.valid() && f.Set<void*>(N::NavModifierAreaClassParam, s_areaCls)) Call(nav, f);
}

// The move timeline and where it stands; false when it, or one of its reads, did not resolve.
bool TimelineReads(void* g, void*& tl, float& length, float& pos, bool& playing) {
    tl = MoveTimeline(g);
    if (!tl) return false;
    void* const playingFn = TimelineVerb(tl, N::TimelineIsPlayingFn);
    if (!playingFn || !TimelineFloat(tl, N::TimelineGetLengthFn, length) ||
        !TimelineFloat(tl, N::TimelineGetPositionFn, pos) || length <= 0.f)
        return false;
    ParamFrame f(playingFn);
    playing = f.valid() && Call(tl, f) && f.Get<bool>(L"ReturnValue");
    return true;
}

bool TimelineSeek(void* tl, float to) {
    void* const setFn = TimelineVerb(tl, N::TimelineSetPositionFn);
    if (!setFn) return false;
    ParamFrame set(setFn);
    return set.valid() && set.Set<float>(N::TimelineNewPositionParam, to) &&
           set.Set<bool>(N::TimelineFireEventsParam, false) && set.Set<bool>(N::TimelineFireUpdateParam, true) &&
           Call(tl, set);
}

void TimelineCall(void* tl, const wchar_t* name) {
    void* const fn = TimelineVerb(tl, name);
    if (!fn) return;
    ParamFrame f(fn);
    if (f.valid()) Call(tl, f);
}

float Clamp01(float v) { return std::fmin(1.f, std::fmax(0.f, v)); }

// Drive a door that stood at rest on the authority to its end, all of it. The game's own load verb, settime, plays the
// mesh montage and moves the timeline and the box; what it leaves mid-way is finished here: the montage put at the end
// of its section, the timeline stopped at its end with its update fired (settime sets it a second in and runs it on,
// so the box would shrink and grow back), `mov` cleared, the swing sound out and the nav area set.
bool DriveToRest(void* g, bool open, SettleReadback* rb = nullptr) {
    void* const settime = Verb(g, N::GarageSetTimeFn);
    void* tl = nullptr;
    float length = 0.f, pos = 0.f;
    bool playing = false;
    if (!settime || !TimelineReads(g, tl, length, pos, playing)) return false;
    WriteOpen(g, open);
    {
        ParamFrame f(settime);
        if (!f.valid() || !f.Set<bool>(N::GarageSetTimeOpenParam, open) || !Call(g, f)) return false;
    }
    TimelineCall(tl, N::TimelineStopFn);
    TimelineSeek(tl, open ? length : 0.f);
    if (g_movOff >= 0) *reinterpret_cast<bool*>(reinterpret_cast<char*>(g) + g_movOff) = false;
    if (void* const loop = ComponentAt(g, g_loopOff)) SetComponentActive(loop, false);
    if (!open) SetClosedNavArea(g);
    if (void* const anim = DoorAnimInstance(g)) {
        if (rb) rb->meshReached = true;
        if (void* const montage = ActiveMontage(anim)) {
            JumpToSectionsEnd(anim, montage, open);
            if (rb) rb->montageRead = MontagePosition(anim, montage, rb->montagePos);
        }
    }
    return true;
}

// Start the animated swing toward `open` and put the timeline and the mesh at the authority's place in it.
bool DriveMidSwing(void* g, bool open, float fraction) {
    void* const acivae = Verb(g, L"acivae");
    void* tl = nullptr;
    float length = 0.f, pos = 0.f;
    bool playing = false;
    if (!acivae || !TimelineReads(g, tl, length, pos, playing)) return false;
    WriteOpen(g, open);
    {
        // acivae takes its direction from the flag just written; ApplyOpen is not used since it returns early
        // on a flag that already matches.
        ParamFrame f(acivae);
        if (!f.valid() || !Call(g, f)) return false;
    }
    const float frac = Clamp01(fraction);
    TimelineSeek(tl, frac * length);
    // A reverse started at the closed end finishes on the spot, so the seek leaves the timeline stopped mid-way.
    if (TimelineReads(g, tl, length, pos, playing) && !playing)
        TimelineCall(tl, open ? N::TimelinePlayFn : N::TimelineReverseFn);
    void* const anim = DoorAnimInstance(g);
    void* const montage = anim ? ActiveMontage(anim) : nullptr;
    float start = 0.f, end = 0.f;
    // The section's length is not read by name: its start is where the swing's montage stands now, its end where a jump
    // to the section's end puts it, and the mesh goes the timeline's progress along that.
    if (montage && MontagePosition(anim, montage, start) && JumpToSectionsEnd(anim, montage, open) &&
        MontagePosition(anim, montage, end))
        SetMontagePosition(anim, montage, start + (end - start) * (open ? frac : 1.f - frac));
    return true;
}

}  // namespace

Settled SettleAtRest(void* g, bool open, SettleReadback* rb) {
    SettleReadback local;
    SettleReadback& r = rb ? *rb : local;
    r = SettleReadback{};
    void* tl = nullptr;
    float length = 0.f, pos = 0.f;
    bool playing = false;
    if (!g || !TimelineReads(g, tl, length, pos, playing)) return NormalizeOpen(g, open, &r.timelineBefore, &r.timelineAfter);
    r.timelineBefore = r.timelineAfter = pos;
    bool moving = false;
    TryReadMoving(g, moving);
    if (moving || playing) return Settled::Busy;
    WriteOpen(g, open);
    constexpr float kEps = 0.01f;
    if (std::fabs(pos - (open ? length : 0.f)) <= kEps) return Settled::Already;
    // Off its end: the same drive a joiner's seed gives a door at rest. The timeline alone moves only the collision
    // box -- the mesh is the montage's -- so setting the timeline by itself left the visible door where a load that
    // ran settime before the mesh had an animation instance put it.
    if (!DriveToRest(g, open, &r)) return NormalizeOpen(g, open, &r.timelineBefore, &r.timelineAfter);
    r.timelineAfter = r.timelineBefore;
    TimelineReads(g, tl, length, r.timelineAfter, playing);
    return Settled::Snapped;
}

bool TryReadTravel(void* g, bool& moving, float& fraction) {
    void* tl = nullptr;
    float length = 0.f, pos = 0.f;
    bool playing = false;
    if (!TimelineReads(g, tl, length, pos, playing)) return false;
    bool mov = false;
    TryReadMoving(g, mov);
    moving = mov || playing;
    fraction = Clamp01(pos / length);
    return true;
}

Seeded SeedTo(void* g, bool open, bool moving, float fraction, uint32_t& cookie) {
    if (!g || !g_resolved.load(std::memory_order_acquire)) return Seeded::Failed;
    void* tl = nullptr;
    float length = 0.f, pos = 0.f;
    bool playing = false;
    if (!TimelineReads(g, tl, length, pos, playing)) return Seeded::Failed;
    // The door mesh needs its animation instance before a montage means anything. A garage whose mesh offset did not
    // resolve is driven without that gate, since settime and acivae play the montage themselves.
    if (g_meshOff >= 0 && !DoorAnimInstance(g)) return Seeded::Working;
    bool mov = false, curOpen = open;
    TryReadMoving(g, mov);
    TryReadOpen(g, curOpen);
    if (cookie & kSeedDriven) {
        if (curOpen == open) {
            if (mov || playing) return Seeded::Working;
            // At rest: the timeline ends where the flag says, and anything short of it (a swing that stopped early, a
            // save's late settime) is brought there.
            NormalizeOpen(g, open);
            return Seeded::Rest;
        }
        cookie = 0;  // the flag was rewritten behind the seed (a late save load): drive it again
    }
    if (!(moving ? DriveMidSwing(g, open, fraction) : DriveToRest(g, open))) return Seeded::Failed;
    cookie |= kSeedDriven;
    return Seeded::Working;
}

}  // namespace ue_wrap::garage
