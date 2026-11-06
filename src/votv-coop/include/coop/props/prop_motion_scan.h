// coop/props/prop_motion_scan.h -- HOST: the driven-prop channel's feeder for motion no verb owns.
//
// The driven-prop channel (prop_drive_host) streams what a verb claimed or coasted. A host prop moved
// for any other reason -- a table settling under the chairs on it, a chair a falling table knocks, a
// crate the host's own body shoves -- had no channel, so each client kept its copy where its own
// physics left it until somebody grabbed the prop. Every kRefreshMs this module snapshots the host's
// own Prop elements and splits them into near-a-player and far; each tick it reads the pose of a
// bounded number of each, round-robin, near first. A simulating prop more than kScanEpsCm/kScanEpsDeg
// from the pose last seen is handed to prop_drive_host::Coast, which streams it until it rests and
// closes with the rest pose, physics flags and velocity. A kinematic prop (frozen, static, tied,
// attached) is another lane's; a driven or held one only has its baseline follow.
//
// Bounded by reads per tick, new coasts per tick and props coasting at once (below the host-birth
// cap). The rows ride the session's pose-turn rotation and quantization. No wire change. Host only;
// a client's Tick is a no-op. Game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace coop::net { class Session; }

namespace coop::prop_motion_scan {

// A prop moved past these since this module last saw it: the converge's own "aligned" bounds
// (remote_prop_spawn), above any resting jitter, well below a visible desync.
constexpr float kScanEpsCm  = 2.0f;
constexpr float kScanEpsDeg = 1.0f;
// Within this of any player a prop is in the near set and is read more often.
constexpr float kNearCm = 3000.0f;
// Reads per tick from each set. At ~60 ticks a second a near set of a few hundred props is swept in
// well under a second; the far set of a heavy base (a few thousand) in about three.
constexpr size_t kNearChecksPerTick = 24;
constexpr size_t kFarChecksPerTick  = 16;
// New coasts per tick, and props coasting at once before this feeder holds off. The host-birth feeder
// refuses at 32 (host_spawn_watcher), so this one stops short of it.
constexpr size_t kMaxNewCoastsPerTick = 4;
constexpr size_t kMaxCoasting         = 24;
// How often the candidate list is rebuilt from the Registry and re-split by distance.
constexpr uint64_t kRefreshMs = 2000;
// A client's row the host released (its End applied to the host's copy) this recently: the copy's motion is the
// release's own settle -- the body simulating again from the End's pose -- not new motion. Coasting it streamed the
// settle back to the client that had just let go and parked that client's copy under its player's feet, so the
// prop changed hands every few seconds (docs/props.md, standing on a prop). The baseline follows meanwhile.
constexpr uint64_t kQuietAfterReleaseMs = 1500;

// --- the pure rules, constexpr so the .cpp checks them at compile time ---

// True when `loc`/`rot` moved past the scan epsilons from the baseline.
inline bool MovedPastScanEps(const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot,
                             const ue_wrap::FVector& base, const ue_wrap::FRotator& baseRot) {
    if (std::fabs(loc.X - base.X) > kScanEpsCm) return true;
    if (std::fabs(loc.Y - base.Y) > kScanEpsCm) return true;
    if (std::fabs(loc.Z - base.Z) > kScanEpsCm) return true;
    if (std::fabs(ue_wrap::NormalizeAxis(rot.Pitch - baseRot.Pitch)) > kScanEpsDeg) return true;
    if (std::fabs(ue_wrap::NormalizeAxis(rot.Yaw   - baseRot.Yaw))   > kScanEpsDeg) return true;
    if (std::fabs(ue_wrap::NormalizeAxis(rot.Roll  - baseRot.Roll))  > kScanEpsDeg) return true;
    return false;
}

// How many coasts this tick may open: the per-tick cap, less what is already coasting past the
// concurrent cap. Never negative.
constexpr size_t CoastSlots(size_t coastingNow, size_t openedThisTick) {
    if (coastingNow >= kMaxCoasting) return 0;
    const size_t room = kMaxCoasting - coastingNow;
    if (openedThisTick >= kMaxNewCoastsPerTick) return 0;
    const size_t tick = kMaxNewCoastsPerTick - openedThisTick;
    return room < tick ? room : tick;
}

// The verdict on one read prop: coast it only when it simulates, is in no other lane's hands, and
// moved past the epsilons since the baseline.
constexpr bool ShouldCoast(bool moved, bool simulating, bool alreadyDriven, bool inHand) {
    return moved && simulating && !alreadyDriven && !inHand;
}

// --- the module ---

// Per host tick, before prop_drive_host::Pump so a coast opened here publishes on the same tick.
// A no-op on a client. Game thread.
void Tick(coop::net::Session& s, bool isHost);

// Coasts opened by this module this session, for the log and the drills.
size_t CoastsOpened();

// Session end: the candidate lists and baselines go. Game thread.
void OnDisconnect();

}  // namespace coop::prop_motion_scan
