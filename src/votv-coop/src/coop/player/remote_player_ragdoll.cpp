// coop/player/remote_player_ragdoll.cpp -- see coop/player/remote_player_ragdoll.h.

#include "coop/player/remote_player_ragdoll.h"

#include "coop/player/skin_effects.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

#include <chrono>

namespace coop {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

namespace {

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Show/hide the puppet's TWO kel body meshes -- the native mesh slot and its
// mesh_playerVisible child, both of which render. VISIBILITY only (SetVisibility,
// propagate=false): the mesh ASSETS are untouched, so a pak-loaded custom-skin asset keeps its
// component reference and cannot be GC'd during the flop, which clearing the mesh would risk by
// dropping the only strong ref. Forcing visible=true on the restore side is a safe no-op when
// nothing was hidden -- visible IS the puppet's normal state, forced at spawn -- so the restore
// needs no bookkeeping.
void SetPuppetKelMeshesVisible(void* puppetActor, bool visible) {
    E::SetSceneComponentVisibility(
        ue_wrap::puppet::GetNativeBodyMeshComponent(puppetActor), visible, /*propagate=*/false);
    E::SetSceneComponentVisibility(
        ue_wrap::puppet::GetMeshPlayerVisibleComponent(puppetActor), visible, /*propagate=*/false);
}

constexpr uint64_t kStartRetryMs = 500;     // throttle for a Start retry within one episode
constexpr uint32_t kMaxStartFails = 20;     // give up retrying after this many failures per episode
constexpr uint64_t kSimCheckMs = 250;       // sim watchdog cadence
// A sender at rest streams ~0 velocity; see SetPose.
constexpr float kRestLinCmS = 5.f;
constexpr float kRestAngDegS = 5.f;

}  // namespace

bool RagdollDisplay::OnWireBit(bool ragdollBit, void* puppetActor, int32_t puppetIdx) {
    if (ragdollBit) {
        if (!wireState_) {
            // Rising edge: a new episode.
            wireState_ = true;
            ++episode_;
            starts_ = stops_ = updates_ = velApplied_ = velSkipped_ = simRearms_ = startFails_ = 0;
            nextRetryMs_ = nextSimCheckMs_ = 0;
            UE_LOGI("RagdollDisplay: episode %u BEGIN (wire bit rose)", episode_);
            Start(puppetActor, puppetIdx);
            return false;
        }
        // The bit is level-triggered (every pose packet repeats it, and a snapshot re-seed or the
        // host relay of a client's pose can resend it): an episode already running with a live
        // body only counts an update, never a respawn.
        if (active_ && body_ && R::IsLiveByIndex(body_, bodyIdx_)) {
            ++updates_;
            return false;
        }
        // The bit is up but there is no live body: the Start failed, or the body was GC-reaped
        // mid-episode (DriveAttached recovered the puppet). Retry in the SAME episode, throttled,
        // so a transient spawn failure does not leave the remote standing for the whole flop.
        const uint64_t now = NowMs();
        if (startFails_ < kMaxStartFails && now >= nextRetryMs_) {
            nextRetryMs_ = now + kStartRetryMs;
            Start(puppetActor, puppetIdx);
        }
        return false;
    }
    if (!wireState_) return false;
    wireState_ = false;
    // Falling edge: ALWAYS run the recover, even when no body is live -- it un-hides the kel
    // meshes and detaches, both safe no-ops on a puppet Start never touched.
    Stop(puppetActor, puppetIdx);
    UE_LOGI("RagdollDisplay: episode %u END starts=%u stops=%u updates=%u velApplied=%u velSkippedAtRest=%u "
            "simRearms=%u startFails=%u",
            episode_, starts_, stops_, updates_, velApplied_, velSkipped_, simRearms_, startFails_);
    return true;
}

bool RagdollDisplay::SetPose(const coop::net::RagdollPoseSnapshot& snap) {
    // Velocity half of the pose stream only: slave the plushie's pelvis rigid body to the sender so
    // the visible flop TRACKS the real ragdoll. The streamed pelvis ROTATION is unused since the
    // visible-plushy rework -- it existed to tumble the kel-attached actor -- and stays on the wire
    // untouched, for protocol stability.
    if (active_ && body_ && R::IsLiveByIndex(body_, bodyIdx_)) {
        // A sender at rest streams ~0 velocity. Overwriting the pelvis velocity with 0 at the
        // stream rate pins the body: gravity never accumulates past one packet interval and the
        // plushie cannot topple or curl, so it would hang upright and rigid. At rest, leave the
        // local simulation free to settle.
        const float lin2 = snap.linVelX * snap.linVelX + snap.linVelY * snap.linVelY + snap.linVelZ * snap.linVelZ;
        const float ang2 = snap.angVelX * snap.angVelX + snap.angVelY * snap.angVelY + snap.angVelZ * snap.angVelZ;
        if (lin2 < kRestLinCmS * kRestLinCmS && ang2 < kRestAngDegS * kRestAngDegS) {
            ++velSkipped_;
            return true;
        }
        E::DriveRagdollBodyPelvisVelocity(
            body_,
            ue_wrap::FVector{snap.linVelX, snap.linVelY, snap.linVelZ},
            ue_wrap::FVector{snap.angVelX, snap.angVelY, snap.angVelZ});
        ++velApplied_;
        return true;
    }
    return false;
}

RagdollDisplay::Drive RagdollDisplay::DriveAttached(void* puppetActor, int32_t puppetIdx) {
    if (!active_) return Drive::Inactive;
    if (body_ && R::IsLiveByIndex(body_, bodyIdx_)) {
        // The VISIBLE plushie body is the whole display; the (hidden-kel) puppet actor just rides
        // the pelvis attach. Nothing to drive per frame except the sim watchdog: a body whose
        // physics is not running stays frozen at its spawn transform (upright and rigid).
        const uint64_t now = NowMs();
        if (now >= nextSimCheckMs_) {
            nextSimCheckMs_ = now + kSimCheckMs;
            bool rearmed = false;
            E::EnsureRagdollBodySimulating(body_, &rearmed);
            if (rearmed) {
                ++simRearms_;
                if (simRearms_ <= 3)
                    UE_LOGW("RagdollDisplay: episode %u body %p was NOT simulating -- sim re-armed (#%u)",
                            episode_, body_, simRearms_);
            }
        }
        return Drive::Attached;
    }
    // Self-heal: the body was GC-killed mid-ragdoll, a level transition having reaped it. Recover
    // NOW -- un-hide, detach, clear -- so the puppet is not stuck invisible on a dead attachment;
    // the owner resumes pose-drive this same tick. While the wire bit stays up, OnWireBit retries
    // a fresh body within the same episode.
    UE_LOGW("RagdollDisplay: episode %u body died under the puppet -- recovering the puppet", episode_);
    Stop(puppetActor, puppetIdx);
    return Drive::StoppedNow;
}

void RagdollDisplay::TeardownForDestroy() {
    // The body is a SEPARATE actor that would otherwise outlive the puppet as
    // an orphan. IsLiveByIndex-guarded so a GC-recycled address isn't mistaken
    // for our body. No detach + no kel un-hide: the puppet actor (and its
    // hidden meshes) is destroyed right after.
    if (body_ && R::IsLiveByIndex(body_, bodyIdx_)) E::DestroyActor(body_);
    body_ = nullptr;
    bodyIdx_ = -1;
    wireState_ = false;  // a recycled puppet starts un-ragdolled + re-converges
    active_ = false;
    nextRetryMs_ = nextSimCheckMs_ = 0;
}

void RagdollDisplay::Start(void* puppetActor, int32_t puppetIdx) {
    if (!puppetActor || !R::IsLiveByIndex(puppetActor, puppetIdx)) return;
    // Idempotent: a live body means this episode is already displayed -- never spawn a second one.
    if (active_ && body_ && R::IsLiveByIndex(body_, bodyIdx_)) return;
    // A stale body reference must not leak: destroy it before the respawn.
    if (body_ && R::IsLiveByIndex(body_, bodyIdx_)) {
        UE_LOGW("RagdollDisplay::Start: episode %u found a leftover body %p -- destroying it before the respawn",
                episode_, body_);
        E::DestroyActor(body_);
    }
    body_ = nullptr;
    bodyIdx_ = -1;

    // Spawn VOTV's own playerRagdoll_C VISIBLE, co-located with the puppet: the game's plushie
    // ragdoll, the full six-bone chain physics and the exact body single-player shows in mirrors.
    // If it fails, leave the puppet UPRIGHT and pose-driving (active_ stays false) -- graceful. The
    // body's Player field points at this puppet, and the spawn is DEATH-FREE: no ragdollMode.
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    if (!E::TryGetActorLocation(puppetActor, loc) || !E::TryGetActorRotation(puppetActor, rot)) {
        UE_LOGW("RagdollDisplay::Start: the puppet's location or rotation could not be read -- puppet stays upright "
                "(graceful)");
        ++startFails_;
        return;
    }
    // Spawn upright: after a KeepWorld detach the puppet's pitch and roll are the previous flop's
    // leftovers, and mean nothing for a fresh body.
    rot.Pitch = 0.f;
    rot.Roll = 0.f;
    void* body = E::SpawnPlayerRagdollBody(puppetActor, loc, rot);
    if (!body) {
        UE_LOGW("RagdollDisplay::Start: playerRagdoll_C spawn failed -- puppet stays upright (graceful)");
        ++startFails_;
        return;
    }
    body_ = body;
    bodyIdx_ = R::InternalIndexOf(body);

    // PELVIS-attach the puppet actor to the body: the position anchor (nameplate +
    // recover hand-off follow the flop). The puppet itself goes INVISIBLE next -- the
    // plushie is the display. If the attach fails, destroy the body + stay upright.
    if (!E::AttachActorToRagdollBody(puppetActor, body)) {
        UE_LOGW("RagdollDisplay::Start: pelvis-attach failed -- destroying body, puppet stays upright");
        E::DestroyActor(body);
        body_ = nullptr; bodyIdx_ = -1;
        ++startFails_;
        return;
    }
    // Hide the standing kel for the flop -- otherwise it double-images over the
    // plushie (the June "plushy on a stick" rejection, inverted: now the plushie is
    // the visible one). The skin effect rig (belly light / joint sparks) hides
    // with it -- floating sparks over an invisible body break the plushie trick.
    SetPuppetKelMeshesVisible(puppetActor, false);
    coop::skin_effects::SetRigVisible(puppetActor, false);
    active_ = true;
    ++starts_;
    nextSimCheckMs_ = NowMs() + kSimCheckMs;
    UE_LOGI("RagdollDisplay::Start: VISIBLE plushie body=%p spawned, puppet pelvis-attached + kel meshes hidden "
            "(episode %u, starts=%u stops=%u updates=%u)", body, episode_, starts_, stops_, updates_);
}

void RagdollDisplay::Stop(void* puppetActor, int32_t puppetIdx) {
    if (puppetActor && R::IsLiveByIndex(puppetActor, puppetIdx)) {
        // Un-hide the kel meshes FIRST (their normal state -- safe no-op if Start
        // never hid them), then detach (KeepWorld -- the puppet stays where the flop
        // left it; the next pose drives it back to the streamed pose) BEFORE
        // destroying the body it's attached to.
        SetPuppetKelMeshesVisible(puppetActor, true);
        coop::skin_effects::SetRigVisible(puppetActor, true);
        E::DetachActorFromRagdollBody(puppetActor);
    }
    // Destroy the physics body (IsLiveByIndex-guarded against a recycled addr).
    if (body_ && R::IsLiveByIndex(body_, bodyIdx_)) E::DestroyActor(body_);
    body_ = nullptr;
    bodyIdx_ = -1;
    const bool wasActive = active_;
    active_ = false;
    if (wasActive) ++stops_;
    UE_LOGI("RagdollDisplay::Stop: kel meshes restored, puppet detached, plushie body destroyed "
            "(episode %u, starts=%u stops=%u updates=%u)", episode_, starts_, stops_, updates_);
}

}  // namespace coop
