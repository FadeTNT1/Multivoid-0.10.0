// coop/player/remote_player_ragdoll.h -- the remote player's ragdoll DISPLAY.
// Gameplay layer (principle 7): engine access via ue_wrap only. Game thread only.
//
// When a remote player ragdolls (wire kStateBitRagdoll), spawn VOTV's own
// playerRagdoll_C body VISIBLE -- the game's plushie ragdoll, whose mesh is
// natively rigged to the full six-bone physics chain and is what single-player
// shows in mirrors -- and HIDE the puppet's two kel body meshes for the flop.
// Hiding is SetVisibility only, never a mesh-asset clear, so a pak-loaded
// custom-skin asset keeps its component reference and cannot be collected
// mid-flop. The puppet actor stays pelvis-attached to the body as the position
// anchor for the nameplate and the recover hand-off. The spawn is DEATH-FREE:
// ragdollMode is globally scoped and would kill the host, so it is never called.
//
// Owned by RemotePlayer (composition): the owner keeps its glue side effects keyed
// off the return values, so this class never reaches back into its state.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop {

class RagdollDisplay {
public:
    // Wire-bit edge detector (called on every pose packet with the streamed
    // kStateBitRagdoll). Spawns the body on the rising edge / recovers on the
    // falling edge -- exactly once per transition (the bit is continuous, so a
    // dropped edge self-heals on the next pose; TeardownForDestroy resets the
    // latch so a recycled puppet re-converges). Returns true when a STOP ran
    // this call: the owner re-bases its presentation yaw on the wire truth
    // (the get-up is a visual discontinuity anyway).
    //
    // One ragdoll episode is one Start and one Stop. A repeated true bit while the body is live
    // is an update (counted, never a respawn), so the pose packet's level-triggered resend, a
    // snapshot re-seed and the host relay of a client's pose are all idempotent. A true bit with no
    // live body (a Start that failed, or a body GC-reaped mid-episode) retries on a short throttle
    // within the same episode.
    bool OnWireBit(bool ragdollBit, void* puppetActor, int32_t puppetIdx);

    // Ragdoll PHYSICS sync (RemotePlayer::SetRagdollPose): slave the live body's
    // pelvis velocity to the sender's streamed state so the plushie tumbles to TRACK
    // the sender's real ragdoll. The snapshot's pelvis ROTATION is not read -- it
    // tumbled an earlier, kel-attached display -- and stays on the wire untouched.
    // Returns true when applied to a live body; a packet racing ahead of the spawn
    // edge is dropped harmless, and the next one applies.
    bool SetPose(const coop::net::RagdollPoseSnapshot& snap);

    // ApplyToEngine head. While ragdolled the VISIBLE plushie body is the whole
    // display and the attachment owns the puppet transform -- pose-driving would fight
    // it; there is nothing to drive per frame. Keeping the kel meshes visible and
    // slaving them to the body with a master pose is not an option either: name-based
    // master-pose couples only four of the six bones, because the visible skin's
    // skeleton has no thighs or lowlegs, so the legs would not follow.
    enum class Drive {
        Inactive,   // not ragdolling -- the owner pose-drives as normal
        Attached,   // attachment owns the transform this tick -- owner returns
        StoppedNow  // body died under us (e.g. level-transition GC): self-
                    // healed (kel un-hidden, detach, clear) -- owner re-bases
                    // its yaw and falls through to normal pose-drive THIS tick
    };
    //
    // Also the sim watchdog: the body's physics is re-asserted if the engine reports its mesh no
    // longer simulating, so a body that was left frozen at the spawn transform (the upright,
    // rigid plushie) cannot stay that way for the episode.
    Drive DriveAttached(void* puppetActor, int32_t puppetIdx);

    // RemotePlayer::Destroy teardown: destroy the BODY only (the puppet actor
    // dies right after -- it would otherwise orphan the body; its hidden kel
    // meshes die with it, so no un-hide) + reset every latch so a recycled
    // puppet starts un-ragdolled and re-converges.
    void TeardownForDestroy();

    // Test/diagnostic seam (forwarded by RemotePlayer accessors).
    bool Active() const { return active_; }
    void* Body() const { return body_; }
    int32_t BodyIdx() const { return bodyIdx_; }
    uint32_t EpisodeId() const { return episode_; }

private:
    void Start(void* puppetActor, int32_t puppetIdx);
    void Stop(void* puppetActor, int32_t puppetIdx);
    // Sample the body's pelvis rotation and fold its change since Start into peakTiltDeg_.
    void SampleTilt();

    // Last WIRE-bit value acted on (edge detector: the spawn/recover dispatch
    // fires once per transition, no retry).
    bool wireState_ = false;
    // The puppet is currently pelvis-attached to a body -> the attachment
    // drives the transform instead of the pose drive.
    bool active_ = false;
    // The spawned (visible) playerRagdoll_C body + its GUObjectArray index
    // (IsLiveByIndex safety across ticks). null/-1 when not ragdolled.
    void*   body_ = nullptr;
    int32_t bodyIdx_ = -1;

    // Episode bookkeeping, for the log line that proves one episode = one Start + one Stop.
    // episode_ counts rising edges of the wire bit (never reset); the counters below are per
    // episode and reset on the rising edge.
    uint32_t episode_ = 0;
    uint32_t starts_ = 0;       // bodies spawned this episode (1 expected; >1 = a heal restart)
    uint32_t stops_ = 0;        // recovers this episode (1 expected)
    uint32_t updates_ = 0;      // repeated true bits absorbed without a respawn
    uint32_t velApplied_ = 0;   // pelvis velocity writes
    uint32_t velSkipped_ = 0;   // stream packets skipped because the sender was at rest
    uint32_t simRearms_ = 0;    // sim watchdog re-asserts
    uint32_t startFails_ = 0;   // failed spawn attempts this episode
    uint64_t nextRetryMs_ = 0;  // throttle for a Start retry
    uint64_t nextSimCheckMs_ = 0;

    // The newest pelvis velocity the sender streamed, kept even while no body is live: the Start
    // kickoff adds it as the fall velocity when it is fresh (a packet that raced the rising edge).
    float   lastLinVel_[3] = {0.f, 0.f, 0.f};
    uint64_t lastPoseMs_ = 0;

    // Proof the plushie fell over: the pelvis pitch/roll at Start, and the largest change of either
    // since (degrees). The pelvis bone's own axes are not world-up, so the raw rotation of a standing
    // plushie is not zero; the change from its standing rotation is what counts.
    bool  haveStartRot_ = false;
    float startPitch_ = 0.f, startRoll_ = 0.f;
    float peakTiltDeg_ = 0.f;
};

}  // namespace coop
