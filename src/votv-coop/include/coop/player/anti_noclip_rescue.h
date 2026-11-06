// coop/player/anti_noclip_rescue.h -- the game's own "you went through a wall" check, answered with a safe place
// in a co-op session instead of the backrooms or the void.
//
// Each tick the local player traces from `lastLoc` to the camera's place against static geometry (ue_wrap/actors/
// anti_noclip.h has the account). On a hit, with canBackrooms() true, the capsule ignoring collision channel 16 and the
// view target the player, the game blacks the screen and travels to a backrooms level, or teleports the player to the
// save slot's playerTransform with no check of what stands there. In a session that is wrong both ways: the travel ends
// the session for the peer that takes it, the saved place is not a place of this run, and the check trips on things a
// single machine never meets (a client walking where the host's world differs, a body thrown by physics).

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::anti_noclip_rescue {

// The seam is the check's own call: canBackrooms() is evaluated every tick by the local player's tick and nowhere else.
// In a session the watch does the check's arithmetic itself (the trace, the body of canBackrooms, the capsule's response,
// the view target). When the game would have fired, it refuses the call's answer (false, so neither branch runs) and
// relocates the player: to the newest standing place this module saw (on the ground, not falling, ragdolled or seated,
// over a floor), re-validated now with a floor under it and head room over it, else to the base landmark the revive and
// the join already use. The player leaves with no velocity, the verb's own rewrite of `lastLoc` and the game's 5 s
// spawn protection, and the moment opens the inventory guard's void window (coop/items/inventory_death_keep). The
// actor and component the line hit, its end points and the destination are logged under [NOCLIP].
// Outside a session the watch passes the call through. Game thread.

// Cache the session and register the canBackrooms watch (idempotent; every net-pump tick).
void Install(coop::net::Session* session);

// Settle the watch; sample the place the local player stands. Every net-pump tick.
void Tick();

// Session teardown: the samples and the counters go, with one summary line.
void OnDisconnect();

// Whether `at` is far below every place the player was seen standing (a body that fell out of the world). False
// until a place has been sampled.
bool IsVoidPosition(const ue_wrap::FVector& at);

// Developer drill: stand the check's line through the world, so the next tick's check fires and the rescue runs
// the way a real trip would. False when there is no session, no local player or the field does not resolve.
bool DevForce();

}  // namespace coop::anti_noclip_rescue
