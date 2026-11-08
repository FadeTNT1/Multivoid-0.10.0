// coop/player/timeout_hold.h -- the local half of an admin's timeout: this machine's player held where
// it stands until a deadline.
//
// The host orders it (coop/moderation/timeout sends a StatOrder with the jail op, stat_orders applies it
// here) because a player's body exists only in its own process. While the deadline stands, each pump tick
// puts the local pawn in MOVE_None (ue_wrap/engine/character_hold: no walking, no jumping; looking around
// stays), and interceptors on the use, drop and fire presses cancel the game's own handlers, so nothing
// is grabbed, used, dropped or fired. Expiry, a release order and the session's end give the pawn
// MOVE_Walking back and leave nothing behind. The host's own player is never ordered.
//
// The deadline is this machine's monotonic clock: the order carries the seconds that remain, so the
// player's clock and the host's never need to agree. Game thread, except the readers.

#pragma once

namespace coop::timeout_hold {

// An order from the host: hold the local player for `seconds` more (a restart when already held), or
// release it when `seconds` is not positive. False when the seconds are not a number.
bool Apply(float seconds);

// Any thread, lock-free: the hold stands.
bool Active();

// Any thread, lock-free: whole seconds left, rounded up; 0 when no hold stands.
int RemainingSecs();

// Every pump tick: hold the pawn, end an expired hold, install the input guards once the player class
// loads. Cheap when no hold stands.
void Tick();

// The session ended: release the pawn and forget the deadline.
void OnDisconnect();

}  // namespace coop::timeout_hold
