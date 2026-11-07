// coop/props/glowstick_hang_sync.cpp -- see coop/props/glowstick_hang_sync.h for the design.

#include "coop/props/glowstick_hang_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/local_streams.h"  // LastHeldActor, CurrentHoldGen: the hold a hang ends
#include "coop/props/prop_drive_host.h"  // OverrideClientSyncer
#include "coop/props/prop_drive_stream.h"  // IsParked: a copy the host's driven-prop stream still drives
#include "coop/props/prop_stick_sync.h"  // FillStickIdentityAndPose
#include "coop/props/remote_prop.h"
#include "ue_wrap/actors/glowstick.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_physics.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace coop::glowstick_hang_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace G  = ue_wrap::glowstick;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowStickTo, "glowstick_hang_sync", L"prop_glowstick_C", L"stickto", Declared);
UE_FACT_FN(kRowRelease, "glowstick_hang_sync", L"prop_glowstick_C", L"releaseglowstick", Declared);

constexpr int     kTagStickTo = 0x474C4F57;  // 'GLOW'
constexpr int     kTagRelease = 0x474C5253;  // 'GLRS'
constexpr uint8_t kHungFlag   = 4;           // see the header: a hang on world geometry

std::atomic<coop::net::Session*> g_session{nullptr};
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg g_reg = Reg::Pending;  // game thread

// Inside our own replay of the stick's release: not mirrored back. Game thread, as the gate's callbacks are.
int g_replayDepth = 0;

coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

struct Pending {
    void*   prop = nullptr;
    int32_t internalIdx = -1;
    bool    release = false;
};
constexpr int kMaxPending = 8;
std::mutex g_pendingMutex;
Pending    g_pending[kMaxPending];
int        g_pendingCount = 0;

// ---- Receiver state, game thread only ----
// hookdelay's body runs after a zero delay, a frame or so after the hang that called it, and rebuilds the constraint
// whenever `stickstatic` is set then. A release replayed before it fires is undone by it (releaseglowstick clears
// `sticked`, not `stickstatic`), so a release that arrives while a hang applied here may still have that delay pending
// waits until it cannot. Clearing `stickstatic` instead is no fix: the delay's other branch constrains the mesh to
// `stickcomp`, which on a copy is null, and a null component pins the mesh to the world.
uint32_t g_pass = 0;  // Tick passes since load

using Clock = std::chrono::steady_clock;
constexpr uint32_t kHookDelayPasses = 3;                               // the delay has fired after this many passes
constexpr auto     kHookDelayTime   = std::chrono::milliseconds(100);  // and this long, whichever is later
constexpr auto     kHangCheckAfter  = std::chrono::milliseconds(1000);
constexpr float    kFellBelowHookCm = 100.f;  // a stick this far below its hook point is not hanging from it
constexpr size_t   kMaxTracked      = 16;

struct RecentHang {
    void*             prop = nullptr;
    int32_t           internalIdx = -1;
    uint32_t          pass = 0;
    Clock::time_point at{};
};
std::vector<RecentHang> g_recentHangs;

struct DeferredRelease {
    void*                            prop = nullptr;
    int32_t                          internalIdx = -1;
    bool                             fromHold = false;  // ReplayRelease's: no pose of a sender's to place
    coop::net::PropStickStatePayload payload{};
    std::wstring                     keyW;
    uint8_t                          slot = 0;
};
std::vector<DeferredRelease> g_deferredReleases;

// A receiver's proof the hang holds: ~1 s after HANG applied, the stick's height against its hook's.
struct HangCheck {
    void*             prop = nullptr;
    int32_t           internalIdx = -1;
    uint32_t          eid = 0;
    std::wstring      keyW;
    ue_wrap::FVector  hookPoint{};
    Clock::time_point due{};
};
std::vector<HangCheck> g_hangChecks;

bool HookDelayMayBePending(void* prop) {
    const auto now = Clock::now();
    for (const RecentHang& h : g_recentHangs)
        if (h.prop == prop && R::IsLiveByIndex(prop, h.internalIdx) &&
            (g_pass - h.pass < kHookDelayPasses || now - h.at < kHookDelayTime))
            return true;
    return false;
}

void NoteHangApplied(void* prop) {
    const RecentHang fresh{prop, R::InternalIndexOf(prop), g_pass, Clock::now()};
    for (RecentHang& h : g_recentHangs)
        if (h.prop == prop) {
            h = fresh;
            return;
        }
    if (g_recentHangs.size() >= kMaxTracked) g_recentHangs.erase(g_recentHangs.begin());
    g_recentHangs.push_back(fresh);
}

void ScheduleHangCheck(void* prop, uint32_t eid, const std::wstring& keyW, const ue_wrap::FVector& hookPoint) {
    for (auto it = g_hangChecks.begin(); it != g_hangChecks.end(); ++it)
        if (it->prop == prop) {
            g_hangChecks.erase(it);
            break;
        }
    if (g_hangChecks.size() >= kMaxTracked) g_hangChecks.erase(g_hangChecks.begin());
    HangCheck c;
    c.prop = prop;
    c.internalIdx = R::InternalIndexOf(prop);
    c.eid = eid;
    c.keyW = keyW;
    c.hookPoint = hookPoint;
    c.due = Clock::now() + kHangCheckAfter;
    g_hangChecks.push_back(std::move(c));
}

void RunDueHangChecks() {
    if (g_hangChecks.empty()) return;
    const auto now = Clock::now();
    for (size_t i = 0; i < g_hangChecks.size();) {
        if (now < g_hangChecks[i].due) {
            ++i;
            continue;
        }
        const HangCheck c = g_hangChecks[i];
        g_hangChecks.erase(g_hangChecks.begin() + static_cast<std::ptrdiff_t>(i));
        if (!c.prop || !R::IsLiveByIndex(c.prop, c.internalIdx)) continue;
        bool sticked = false;
        const bool stickedRead = G::ReadSticked(c.prop, sticked);
        ue_wrap::FVector stick{};
        ue_wrap::FVector hook{};
        const bool stickRead = E::TryGetActorLocation(c.prop, stick);
        const bool hookRead = G::ReadHookWorldLocation(c.prop, hook);
        if (!stickRead || !hookRead) {
            UE_LOGW("glowstick_hang: HANG check +1s key='%ls' eid=%u -- the stick (%d) or hook (%d) location did "
                    "not read", c.keyW.c_str(), c.eid, stickRead ? 1 : 0, hookRead ? 1 : 0);
            continue;
        }
        const bool up = stick.Z >= hook.Z - kFellBelowHookCm;
        UE_LOGI("glowstick_hang: HANG check +1s key='%ls' eid=%u stick z=%.0f hook z=%.0f (sent hook z=%.0f) "
                "dz=%.0f sticked=%s simulating=%d parked=%d -- %s", c.keyW.c_str(), c.eid, stick.Z, hook.Z,
                c.hookPoint.Z, stick.Z - hook.Z, stickedRead ? (sticked ? "1" : "0") : "?",
                E::IsActorRootSimulatingPhysics(c.prop) ? 1 : 0, coop::prop_drive_stream::IsParked(c.prop) ? 1 : 0,
                up ? "STAYS UP" : "FELL below its hook");
    }
}

void Record(void* prop, bool release) {
    Pending p{};
    p.prop = prop;
    p.internalIdx = R::InternalIndexOf(prop);
    p.release = release;
    std::lock_guard<std::mutex> lk(g_pendingMutex);
    for (int i = 0; i < g_pendingCount; ++i)
        if (g_pending[i].prop == prop && g_pending[i].release == release) return;
    if (g_pendingCount >= kMaxPending) {
        static std::atomic<uint32_t> sDropped{0};
        UE_LOGW("glowstick_hang: pending full -- dropping a %s (#%u)", release ? "release" : "hang",
                sDropped.fetch_add(1, std::memory_order_relaxed) + 1);
        return;
    }
    g_pending[g_pendingCount++] = p;
}

// POST on stickto, whatever called it (the stick's own tick while it is held): the body has run, so the members
// are final; the next pass reads them.
void OnStickToPost(const sg::Call& c) {
    if (!c.object || g_replayDepth > 0) return;
    auto* s = LoadSession();
    if (!s || !s->running() || !s->connected()) return;  // SP: nothing to mirror
    Record(c.object, /*release=*/false);
}

// PRE on releaseglowstick, whatever called it: a grab verb, a drop, the pry of a hold. A stick not hung has nothing
// to mirror, and neither has this peer's own replay.
sg::Verdict OnReleasePre(const sg::Call& c) {
    if (!c.object || g_replayDepth > 0) return sg::Verdict::Run;
    auto* s = LoadSession();
    if (!s || !s->running() || !s->connected()) return sg::Verdict::Run;
    bool sticked = false;
    if (!G::ReadSticked(c.object, sticked) || !sticked) return sg::Verdict::Run;
    Record(c.object, /*release=*/true);
    return sg::Verdict::Run;
}

// The hang message for `prop`, which must be hung on geometry. False (and logged) when the stick is not mirrorable.
bool FillHang(coop::net::PropStickStatePayload& p, void* prop, std::wstring& keyW) {
    bool sticked = false;
    bool stat = false;
    if (!G::ReadSticked(prop, sticked) || !sticked) return false;
    if (!G::ReadStickStatic(prop, stat)) {
        UE_LOGW("glowstick_hang: stickstatic did not resolve on %p -- the hang is not mirrored", prop);
        return false;
    }
    if (!stat) {
        static bool sSaid = false;
        if (!sSaid) {
            sSaid = true;
            UE_LOGW("glowstick_hang: a stick on a SIMULATING prop (stickstatic=false, %p) needs the hit component "
                    "and is not mirrored; its copy on the other peers is released as a drop (said once)", prop);
        }
        return false;
    }
    ue_wrap::FVector hook{};
    if (!G::ReadHookPoint(prop, hook)) {
        UE_LOGW("glowstick_hang: the hook point (loc) did not resolve on %p -- the hang is not mirrored", prop);
        return false;
    }
    if (!coop::prop_stick_sync::FillStickIdentityAndPose(p, prop, keyW)) {
        UE_LOGW("glowstick_hang: hung stick %p has neither key nor eid, or no readable pose -- not broadcast", prop);
        return false;
    }
    p.flags = kHungFlag;
    p.holdGen = (coop::local_streams::LastHeldActor() == prop) ? coop::local_streams::CurrentHoldGen() : uint16_t{0};
    p.linVelX = hook.X;
    p.linVelY = hook.Y;
    p.linVelZ = hook.Z;
    return true;
}

void BroadcastHang(coop::net::Session* s, void* prop) {
    coop::net::PropStickStatePayload p{};
    std::wstring keyW;
    if (!FillHang(p, prop, keyW)) return;
    s->SendReliable(coop::net::ReliableKind::PropStickState, &p, sizeof(p));
    UE_LOGI("glowstick_hang: broadcast HANG key='%ls' eid=%u hook=(%.0f,%.0f,%.0f) pose=(%.0f,%.0f,%.0f) holdGen=%u",
            keyW.c_str(), p.elementId, p.linVelX, p.linVelY, p.linVelZ, p.locX, p.locY, p.locZ,
            static_cast<unsigned>(p.holdGen));
}

void BroadcastRelease(coop::net::Session* s, void* prop) {
    coop::net::PropStickStatePayload p{};
    std::wstring keyW;
    if (!coop::prop_stick_sync::FillStickIdentityAndPose(p, prop, keyW)) {
        UE_LOGW("glowstick_hang: released stick %p has neither key nor eid, or no readable pose -- not broadcast",
                prop);
        return;
    }
    p.flags = 0;
    p.holdGen = 0;
    const ue_wrap::prop::VelocityState v = ue_wrap::prop::GetPhysicsVelocity(prop);
    if (v.ok) {
        p.linVelX = v.linearCmS.X;   p.linVelY = v.linearCmS.Y;   p.linVelZ = v.linearCmS.Z;
        p.angVelX = v.angularDegS.X; p.angVelY = v.angularDegS.Y; p.angVelZ = v.angularDegS.Z;
    }
    s->SendReliable(coop::net::ReliableKind::PropStickState, &p, sizeof(p));
    UE_LOGI("glowstick_hang: broadcast RELEASE key='%ls' eid=%u pose=(%.0f,%.0f,%.0f) linVel=(%.0f,%.0f,%.0f)",
            keyW.c_str(), p.elementId, p.locX, p.locY, p.locZ, p.linVelX, p.linVelY, p.linVelZ);
}

void ApplyReleaseNow(void* prop, const coop::net::PropStickStatePayload& p, const std::wstring& keyW, uint8_t slot);

void ReplayReleaseNow(void* actor) {
    bool sticked = false;
    if (!G::ReadSticked(actor, sticked) || !sticked) return;
    ++g_replayDepth;
    const bool ok = G::CallRelease(actor);
    --g_replayDepth;
    UE_LOGI("glowstick_hang: a peer's hold began on a stick hung here -- releaseglowstick %s", ok ? "ran" : "did NOT run");
}

// A release of this stick that waits on an earlier hang's hookdelay runs now: ahead of a newer hang, in arrival
// order, or once the delay can no longer be pending. A no-op when none waits.
void FlushDeferredReleaseFor(void* prop) {
    for (size_t i = 0; i < g_deferredReleases.size(); ++i) {
        if (g_deferredReleases[i].prop != prop) continue;
        const DeferredRelease d = g_deferredReleases[i];
        g_deferredReleases.erase(g_deferredReleases.begin() + static_cast<std::ptrdiff_t>(i));
        if (!R::IsLiveByIndex(d.prop, d.internalIdx)) return;
        if (d.fromHold) ReplayReleaseNow(d.prop);
        else            ApplyReleaseNow(d.prop, d.payload, d.keyW, d.slot);
        return;
    }
}

// Keep `d` until its stick's hookdelay has fired; a later release of the same stick replaces it.
void DeferRelease(DeferredRelease d) {
    for (DeferredRelease& q : g_deferredReleases)
        if (q.prop == d.prop) {
            q = std::move(d);
            return;
        }
    if (g_deferredReleases.size() >= kMaxTracked) {
        UE_LOGW("glowstick_hang: deferred releases full -- the oldest is applied now");
        FlushDeferredReleaseFor(g_deferredReleases.front().prop);
    }
    g_deferredReleases.push_back(std::move(d));
}

void ApplyHang(void* prop, const coop::net::PropStickStatePayload& p, const std::wstring& keyW, uint8_t slot) {
    FlushDeferredReleaseFor(prop);
    // The sticking peer was holding it, so its pose stream was driving this copy: stop the drive (a cache clear, no
    // physics change), pose the copy where the stick hit, and simulate it, since a constraint holds only a body
    // that moves. A copy the host's driven-prop stream still has parked stays kinematic: the park re-latches a copy
    // switched to simulating, and the stream's End gives the physics back.
    coop::remote_prop::ClearAnyDriveFor(prop);
    E::SetActorLocation(prop, ue_wrap::FVector{p.locX, p.locY, p.locZ});
    E::SetActorRotation(prop, ue_wrap::FRotator{p.rotPitch, p.rotYaw, p.rotRoll});
    const bool parked = coop::prop_drive_stream::IsParked(prop);
    if (!parked) E::SetActorSimulatePhysics(prop, true);
    const ue_wrap::FVector hookPoint{p.linVelX, p.linVelY, p.linVelZ};
    const bool ok = G::CallHang(prop, hookPoint);
    if (ok) {
        NoteHangApplied(prop);
        ScheduleHangCheck(prop, p.elementId, keyW, hookPoint);
        UE_LOGI("glowstick_hang: HANG applied key='%ls' eid=%u hook=(%.0f,%.0f,%.0f) (stickto's tail replayed: "
                "hook detached and placed, hookdelay ran; %s; slot %u)", keyW.c_str(), p.elementId, p.linVelX,
                p.linVelY, p.linVelZ,
                parked ? "parked by the driven-prop stream, its End gives the physics back" : "simulating", slot);
    } else {
        UE_LOGW("glowstick_hang: HANG key='%ls' eid=%u -- the stick's members or hookdelay did not resolve here; the "
                "copy stays free", keyW.c_str(), p.elementId);
    }
}

void ApplyRelease(void* prop, const coop::net::PropStickStatePayload& p, const std::wstring& keyW, uint8_t slot) {
    if (HookDelayMayBePending(prop)) {
        DeferredRelease d;
        d.prop = prop;
        d.internalIdx = R::InternalIndexOf(prop);
        d.payload = p;
        d.keyW = keyW;
        d.slot = slot;
        DeferRelease(std::move(d));
        UE_LOGI("glowstick_hang: RELEASE key='%ls' eid=%u deferred -- the hang applied here just now still has "
                "hookdelay pending, which would rebuild the constraint after it (slot %u)", keyW.c_str(),
                p.elementId, slot);
        return;
    }
    ApplyReleaseNow(prop, p, keyW, slot);
}

void ApplyReleaseNow(void* prop, const coop::net::PropStickStatePayload& p, const std::wstring& keyW, uint8_t slot) {
    bool sticked = false;
    const bool wasHung = G::ReadSticked(prop, sticked) && sticked;
    if (wasHung) {
        ++g_replayDepth;
        const bool ok = G::CallRelease(prop);
        --g_replayDepth;
        if (!ok) {
            UE_LOGW("glowstick_hang: RELEASE key='%ls' eid=%u -- releaseglowstick did not run here; the copy stays "
                    "as it is", keyW.c_str(), p.elementId);
            return;
        }
    }
    // Unless another owner moves the copy -- a peer's hold, the host's driven-prop channel, this peer's own grab --
    // it lets go from the releasing peer's pose with that peer's velocity.
    const bool owned = coop::remote_prop::IsOwnedHere(prop);
    if (!owned) {
        E::SetActorLocation(prop, ue_wrap::FVector{p.locX, p.locY, p.locZ});
        E::SetActorRotation(prop, ue_wrap::FRotator{p.rotPitch, p.rotYaw, p.rotRoll});
        E::SetActorSimulatePhysics(prop, true);
        if (void* mesh = ue_wrap::prop::GetStaticMesh(prop)) {
            E::SetComponentLinearVelocity(mesh, p.linVelX, p.linVelY, p.linVelZ);
            E::SetComponentAngularVelocity(mesh, p.angVelX, p.angVelY, p.angVelZ);
        }
    }
    UE_LOGI("glowstick_hang: RELEASE applied key='%ls' eid=%u (was %s here; %s; slot %u)", keyW.c_str(),
            p.elementId, wasHung ? "hung" : "already free",
            owned ? "another hold moves it" : "let go from the sender's pose", slot);
}

struct ClassCollect {
    std::vector<void*> classes;
    void* base = nullptr;
};
void CollectClass(void* ctx, void* cls, void* /*anyInstance*/) {
    auto* c = static_cast<ClassCollect*>(ctx);
    void* bases[] = {c->base};
    if (R::IsDescendantOfAny(cls, bases, 1)) c->classes.push_back(cls);
}
struct SendCtx {
    coop::net::Session* session = nullptr;
    int slot = 0;
    int sent = 0;
    int seen = 0;
};
void SendIfHung(void* ctx, void* obj, int32_t /*index*/) {
    auto* c = static_cast<SendCtx*>(ctx);
    ++c->seen;
    bool sticked = false;
    if (!G::ReadSticked(obj, sticked) || !sticked) return;
    coop::net::PropStickStatePayload p{};
    std::wstring keyW;
    if (!FillHang(p, obj, keyW)) return;
    p.holdGen = 0;  // no hold of the joiner's to close
    if (c->session->SendReliableToSlot(c->slot, coop::net::ReliableKind::PropStickState, &p, sizeof(p))) {
        ++c->sent;
        UE_LOGI("glowstick_hang: join replay slot %d HANG key='%ls' eid=%u hook=(%.0f,%.0f,%.0f)", c->slot,
                keyW.c_str(), p.elementId, p.linVelX, p.linVelY, p.linVelZ);
    }
}

// The releases that waited out a hang's hookdelay, then the hang checks that are due. Game thread.
void TickReceiver() {
    ++g_pass;
    for (size_t i = 0; i < g_deferredReleases.size();) {
        void* prop = g_deferredReleases[i].prop;
        if (!R::IsLiveByIndex(prop, g_deferredReleases[i].internalIdx)) {
            g_deferredReleases.erase(g_deferredReleases.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (HookDelayMayBePending(prop)) {
            ++i;
            continue;
        }
        FlushDeferredReleaseFor(prop);  // erases entry i; the next one moves into it
    }
    RunDueHangChecks();
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!session || g_reg != Reg::Pending) return;
    const bool a = sg::WatchName(kRowStickTo, kTagStickTo, nullptr, &OnStickToPost);
    const bool b = sg::WatchName(kRowRelease, kTagRelease, &OnReleasePre, nullptr);
    g_reg = (a && b) ? Reg::Registered : Reg::Refused;
    UE_LOGI("glowstick_hang: watches %s (stickto post=%d, releaseglowstick pre=%d)",
            g_reg == Reg::Registered ? "registered" : "REFUSED -- a hung glowstick is not mirrored", a ? 1 : 0,
            b ? 1 : 0);
}

void Tick() {
    TickReceiver();
    Pending ready[kMaxPending];
    int nReady = 0;
    {
        std::lock_guard<std::mutex> lk(g_pendingMutex);
        if (g_pendingCount == 0) return;
        nReady = g_pendingCount;
        for (int i = 0; i < g_pendingCount; ++i) ready[i] = g_pending[i];
        g_pendingCount = 0;
    }
    auto* s = LoadSession();
    if (!s || !s->connected()) return;
    for (int i = 0; i < nReady; ++i) {
        void* prop = ready[i].prop;
        if (!prop || !R::IsLiveByIndex(prop, ready[i].internalIdx)) continue;  // died since the record
        bool sticked = false;
        if (!G::ReadSticked(prop, sticked)) continue;
        if (ready[i].release) {
            if (sticked) continue;  // the body refused: still hung, nothing to mirror
            BroadcastRelease(s, prop);
        } else {
            if (!sticked) continue;  // stickto's body bailed
            BroadcastHang(s, prop);
        }
    }
}

bool OnStickState(const coop::net::PropStickStatePayload& payload, void* prop, const std::wstring& keyW,
                  uint8_t senderPeerSlot) {
    if (!G::IsGlowstick(prop)) return false;
    const bool release = payload.flags == 0;
    if (!release && (payload.flags & kHungFlag) == 0) {
        UE_LOGW("glowstick_hang: key='%ls' eid=%u carries flags=%u, which a glowstick does not take -- dropped",
                keyW.c_str(), payload.elementId, static_cast<unsigned>(payload.flags));
        return true;
    }
    // Both halves write the copy's pose or physics, so a client that syncs the prop loses it first, as to a host verb.
    coop::prop_drive_host::OverrideClientSyncer(prop, release ? "glowstick release" : "glowstick hang");
    if (release) ApplyRelease(prop, payload, keyW, senderPeerSlot);
    else         ApplyHang(prop, payload, keyW, senderPeerSlot);
    return true;
}

void ReplayRelease(void* actor) {
    if (HookDelayMayBePending(actor)) {
        bool sticked = false;
        if (!G::ReadSticked(actor, sticked) || !sticked) return;
        DeferredRelease d;
        d.prop = actor;
        d.internalIdx = R::InternalIndexOf(actor);
        d.fromHold = true;
        DeferRelease(std::move(d));
        UE_LOGI("glowstick_hang: a peer's hold began on a stick hung here just now -- releaseglowstick deferred "
                "until the hang's hookdelay has fired");
        return;
    }
    ReplayReleaseNow(actor);
}

void ConnectReplayForSlot(int slot) {
    auto* s = LoadSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    ClassCollect classes;
    classes.base = ue_wrap::object_index::ClassByName(ue_wrap::profile::name::GlowstickClass);
    if (!classes.base) return;
    ue_wrap::object_index::ForEachClass(&CollectClass, &classes);
    SendCtx ctx;
    ctx.session = s;
    ctx.slot = slot;
    for (void* cls : classes.classes) ue_wrap::object_index::ForEachInstance(cls, &SendIfHung, &ctx);
    UE_LOGI("glowstick_hang: join replay slot %d -- %d glowstick(s) seen, %d hung stick(s) sent", slot, ctx.seen,
            ctx.sent);
}

void OnDisconnect() {
    {
        std::lock_guard<std::mutex> lk(g_pendingMutex);
        g_pendingCount = 0;
    }
    // Game thread (DisconnectAll): the receiver's waits go with the session.
    g_recentHangs.clear();
    g_deferredReleases.clear();
    g_hangChecks.clear();
}

}  // namespace coop::glowstick_hang_sync
