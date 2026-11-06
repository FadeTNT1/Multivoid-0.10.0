// coop/world/achievement_sync.h -- shared achievements: every player of a session ends up with the union of
// the completed achievements (the player's profile) and advancements (the world's save slot), both ways, a
// late joiner included.
//
// The host owns a monotonic completed set. A machine's own completions are seen at the game's own progress
// verbs (a script-gate watch on save_main_C::progressAchievement and saveSlot_C::prAdvanc marks the store
// changed; the store's completed flags are then read, never the call's arguments). A client sends each
// completion it did not receive as an AchievementClaim; the host checks the row against its game's catalogue,
// merges it, completes it on itself, and broadcasts AchievementState to everyone, the claimant included. A
// receiver completes a row it lacks through the game's own verb (popup, stored arrays, laptop list), and keeps a
// row it cannot reach yet until its store loads. Only completion travels: a counter's partial progress stays
// local.
//
// The award a completion pays is the game's own addPoints, into the world's balance. A fresh completion pays the
// host's balance once, through the host's own catalogue row (never an amount from the wire), and the balance
// reaches everyone on BalanceSync; a receiver's apply holds the script gate's cancel on addPoints, so a replayed
// or mirrored completion pays nothing. Achievements persist into the receiver's profile (data10.sav); advancements
// are the hosted world's state and are never uploaded from a client's world.

#pragma once

#include <cstdint>

#include "coop/net/protocol.h"

namespace coop::net { class Session; }

namespace coop::achievement_sync {

// Store the session and register the script-gate watches. Idempotent. Game thread.
void Install(coop::net::Session* session);

// Per-net-pump tick: the watches until they settle, a scan of the local stores when a progress verb ran, the
// queued applies, the queued sends, and the batched profile save. Game thread.
void Tick();

// HOST, at a joiner's world-ready (ConnectReplayForSlot): the whole shared set to the slot. A send that cannot be
// made is owed and retried by Tick until it is, or the slot is gone. Game thread.
void SendUnionToSlot(int slot);

// Wire handlers. A payload that is not well formed is counted and dropped by the dispatcher.
bool WellFormed(const coop::net::AchievementBatchPayload& p);
void CountDropped();

// HOST: a client's completions. Each row is checked against the host's catalogue and merged; a new one is applied
// on the host and broadcast. `slot` is the claimant's seat.
void OnClaim(const coop::net::AchievementBatchPayload& p, uint8_t slot);

// CLIENT: the host's completions, applied as the stores allow.
void OnState(const coop::net::AchievementBatchPayload& p);

// CLIENT, at this client's ClientWorldReady announce: the previous world's advancements are not this world's, so
// their pending applies and the advancement baseline go. Game thread.
void OnClientWorldReady();

// HOST: a departed slot is owed nothing. Game thread.
void OnPeerGone(uint8_t slot);

// Session teardown: the shared set, the queues and the baselines. Game thread.
void OnDisconnect();

}  // namespace coop::achievement_sync
