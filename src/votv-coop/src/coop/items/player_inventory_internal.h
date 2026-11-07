// coop/items/player_inventory_internal.h -- what player_inventory_sync.cpp lends its sibling
// player_inventory_pocket.cpp (a pocket trip's carried state), and what that sibling lends back.
// Internal to coop/items (src tree, not include/). Every name here is game-thread state unless it
// says otherwise.

#pragma once

#include "coop/items/player_inventory_sync.h"  // JoinPlacement, CarriedApply
#include "coop/items/player_profile.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"  // kMaxPeers
#include "ue_wrap/actors/vitals.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::net { class Session; }

namespace coop::player_inventory_sync::detail {

using Clock = std::chrono::steady_clock;

// ---- player_inventory_sync.cpp's ----
extern std::atomic<coop::net::Session*> g_session;  // any thread
// Host: the per-slot pushed-this-connection latch of the on-join profile push.
extern std::array<bool, coop::net::kMaxPeers> g_applySentToSlot;
// Client: this world was built from a host profile (the stream's gate).
extern bool g_profileApplied;
// Client: the last place the local player was standing, and when the sampler took it (a flush that
// writes the departure there leaves the time alone).
extern coop::player_profile::Pose g_standingPose;
extern Clock::time_point g_standingPoseAt;
// Client: the host's apply blob, held for the next join's save object.
extern coop::blob_chunks::Assembler g_clientAssembler;
extern coop::player_profile::Profile g_pendingApply;
extern std::atomic<bool> g_hasPendingApply;  // any thread
// Client: where the join's world appearance puts this player.
extern coop::player_profile::Pose g_joinPose;
extern JoinPlacement g_joinPlacement;
// Client: the join's inventory substitution, carried out as the world loads.
extern bool g_subPending;
extern bool g_subAfterLoad;
extern Clock::time_point g_subRepeatUntil;
extern bool g_subFromProfile;
extern bool g_subRefuseEmpty;
extern std::vector<uint8_t> g_subBytes;
// Client: the pawn's survival vitals, waiting for the local player to load.
extern bool g_pawnVitalsPending;
extern ue_wrap::vitals::Snapshot g_pawnVitals;

// Client: send one built profile (the items blob plus the vitals and g_standingPose) to the host.
bool SendProfile(coop::net::Session* s, std::vector<uint8_t> blob, uint64_t itemsHash, size_t carried,
                 Clock::time_point now);
// Client: carry out the armed substitution on `player`.
void Substitute(void* player, const char* when);

// ---- player_inventory_pocket.cpp's ----
// The trip's carried-state apply, moved by Substitute.
extern CarriedApply g_carriedApply;
// The return's load is armed (ClientRearmForReturn): the next join apply places the player at
// g_returnPose; `g_returnSeeded` says the pending apply is this client's own capture.
extern bool g_returnArmed;
extern bool g_returnSeeded;
extern coop::player_profile::Pose g_returnPose;

}  // namespace coop::player_inventory_sync::detail
