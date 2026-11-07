// coop/props/glowstick_hang_sync.cpp -- see coop/props/glowstick_hang_sync.h for the design.

#include "coop/props/glowstick_hang_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/local_streams.h"  // LastHeldActor, CurrentHoldGen: the hold a hang ends
#include "coop/props/prop_drive_host.h"  // OverrideClientSyncer
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
#include <cmath>
#include <cstdint>
#include <mutex>
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

void ApplyHang(void* prop, const coop::net::PropStickStatePayload& p, const std::wstring& keyW, uint8_t slot) {
    // The sticking peer was holding it, so its pose stream was driving this copy: stop the drive (a cache clear, no
    // physics change), pose the copy where the stick hit, and simulate it, since a constraint holds only a body
    // that moves.
    coop::remote_prop::ClearAnyDriveFor(prop);
    E::SetActorLocation(prop, ue_wrap::FVector{p.locX, p.locY, p.locZ});
    E::SetActorRotation(prop, ue_wrap::FRotator{p.rotPitch, p.rotYaw, p.rotRoll});
    E::SetActorSimulatePhysics(prop, true);
    const bool ok = G::CallHang(prop, ue_wrap::FVector{p.linVelX, p.linVelY, p.linVelZ});
    if (ok) {
        UE_LOGI("glowstick_hang: HANG applied key='%ls' eid=%u hook=(%.0f,%.0f,%.0f) (stickto's tail replayed: "
                "hook placed, hookdelay ran; slot %u)", keyW.c_str(), p.elementId, p.linVelX, p.linVelY,
                p.linVelZ, slot);
    } else {
        UE_LOGW("glowstick_hang: HANG key='%ls' eid=%u -- the stick's members or hookdelay did not resolve here; the "
                "copy stays free", keyW.c_str(), p.elementId);
    }
}

void ApplyRelease(void* prop, const coop::net::PropStickStatePayload& p, const std::wstring& keyW, uint8_t slot) {
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
    bool sticked = false;
    if (!G::ReadSticked(actor, sticked) || !sticked) return;
    ++g_replayDepth;
    const bool ok = G::CallRelease(actor);
    --g_replayDepth;
    UE_LOGI("glowstick_hang: a peer's hold began on a stick hung here -- releaseglowstick %s", ok ? "ran" : "did NOT run");
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
    std::lock_guard<std::mutex> lk(g_pendingMutex);
    g_pendingCount = 0;
}

}  // namespace coop::glowstick_hang_sync
