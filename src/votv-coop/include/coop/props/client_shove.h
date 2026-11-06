// coop/props/client_shove.h -- CLIENT: a prop this client's own body shoves becomes a prop the client syncs.
//
// The host's motion scan (prop_motion_scan) reads the host's own props only, so a loose prop a CLIENT walks into moved
// on that client's screen and nowhere else: the host's copy stayed put, and when the host next moved the prop
// (a coast, a settle, another player's touch) the client's copy snapped back to the host's place. The prop's syncer
// election already exists for a client's kick (prop_syncer::Claim, one PropSyncClaim per episode, the host's answer,
// then the copy's pose on the drive channel while it keeps simulating here); what was missing is a feeder that notices
// a shove. This is that feeder, the client-side counterpart of the host scan:
//
//   - every kRefreshMs it takes the host-allocated Prop elements this client has, near the local pawn (within kNearCm)
//     and far, and each tick reads a bounded number of each, near first, against a baseline pose;
//   - a prop that moved past kShoveEpsCm / kShoveEpsDeg, simulates physics, lies within kShoveRangeCm of the local
//     pawn, and is in no other lane's hands (a hand, the host's stream, a throw, a claim already open, a host stream's
//     end less than kQuietAfterEndMs ago, whose own settling is not a shove) is claimed with PropSyncReason::LocalShove;
//   - claims are paced (kMaxClaimsPerWindow per kWindowMs, one per prop per kReclaimMs) and the host still judges every
//     one: a prop someone holds, a host verb's row, another client's row and a prop out of the sender's reach are
//     refused there, so a shove never steals a prop.
//
// Client only; the host's Tick is a no-op. `client_shove=0` switches it off. Game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace coop::net { class Session; }

namespace coop::client_shove {

// A prop moved past these since this module last saw it: a little above the host scan's 2 cm / 1 deg, since the same
// client's own settling jitters more than the host's.
constexpr float kShoveEpsCm  = 4.0f;
constexpr float kShoveEpsDeg = 3.0f;
// A prop is a shove candidate only this close to the local pawn's centre, and in the read-first set this close.
constexpr float kShoveRangeCm = 300.0f;
constexpr float kNearCm = 800.0f;
// Reads per tick from each set, near first.
constexpr size_t kNearChecksPerTick = 16;
constexpr size_t kFarChecksPerTick  = 12;
// The candidate lists are rebuilt this often.
constexpr uint64_t kRefreshMs = 1000;
// Claim pacing: at most kMaxClaimsPerWindow in any kWindowMs, and one per prop per kReclaimMs.
constexpr uint32_t kMaxClaimsPerWindow = 3;
constexpr uint64_t kWindowMs  = 1000;
constexpr uint64_t kReclaimMs = 3000;
// A host stream's End applied to a prop this recently is that prop settling, not a shove.
constexpr uint64_t kQuietAfterEndMs = 2500;

// --- the pure rules, constexpr so the .cpp checks them at compile time ---

// True when `loc`/`rot` moved past the shove epsilons from the baseline.
inline bool MovedPastShoveEps(const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot,
                              const ue_wrap::FVector& base, const ue_wrap::FRotator& baseRot) {
    if (std::fabs(loc.X - base.X) > kShoveEpsCm) return true;
    if (std::fabs(loc.Y - base.Y) > kShoveEpsCm) return true;
    if (std::fabs(loc.Z - base.Z) > kShoveEpsCm) return true;
    if (std::fabs(ue_wrap::NormalizeAxis(rot.Pitch - baseRot.Pitch)) > kShoveEpsDeg) return true;
    if (std::fabs(ue_wrap::NormalizeAxis(rot.Yaw   - baseRot.Yaw))   > kShoveEpsDeg) return true;
    if (std::fabs(ue_wrap::NormalizeAxis(rot.Roll  - baseRot.Roll))  > kShoveEpsDeg) return true;
    return false;
}

// True when a point at squared distance `d2` from the pawn is within `range`.
constexpr bool Within(float d2, float range) { return d2 <= range * range; }

// Whether another claim may go out now: `sentInWindow` claims were sent in the current window.
constexpr bool ClaimAllowed(uint32_t sentInWindow) { return sentInWindow < kMaxClaimsPerWindow; }

// The verdict on a prop that moved: a shove only when it simulates, is near the pawn and is no other lane's.
constexpr bool IsShove(bool simulating, bool nearPawn, bool otherLane) { return simulating && nearPawn && !otherLane; }

// --- the module ---

// Per client gameplay tick, before prop_syncer's tick in the driven-prop pump. A no-op on the host. Game thread.
void Tick(coop::net::Session& s, bool isHost);

// Claims sent by this module this session, for the log and the drills.
size_t ClaimsSent();

// Session end: the candidate lists, baselines and pacing go. Game thread.
void OnDisconnect();

}  // namespace coop::client_shove
