// coop/world/basement_flood_sync.h -- the basement's water level is the host's, and every peer sees it rise and drain.
//
// The basement holds one water volume (waterVolume_basementFlooder_C), whose level is the actor's distance from its dry
// position. The game moves it from three writers that every peer ran on its own: the drain's tick (addWater, negative
// power: it falls), a burst pipe's tick (addWater, positive power: it rises) and the cheat menu's `bflood`
// (startMoving). The host's basement flooded and a client's stayed dry, and a client walking where the host's water was
// not held a different swimming state. The host owns the level. On a client the addWater and startMoving bodies are
// refused at the script gate, per call and only for the game's own callers: a removed second author, not a blindfold,
// since the level they would write is the replicated one (the drain's blockage count and particles still run).

#pragma once

#include "coop/net/protocol.h"

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::basement_flood_sync {

// The host sends the level (BasementFloodState) about twice a second while it moves, once when it settles, and to a
// joiner at its ready edge (a level that arrives before the water volume has streamed in waits for it). A client
// places the water with the actor's own setLevel and refreshes its data with the parent's updateWaterData, so the
// surface, the depth, the buoyancy boxes and the depths the player and the props track follow it. The power panel's
// `waterlogged` is not sent: the drain's own timer derives it from the level on every peer. Game thread.

// Cache the session and register the two script-gate watches (idempotent; every net-pump tick).
void Install(coop::net::Session* session);

// Settle the watches; HOST: send the level when it moved or settled; CLIENT: land a level that waited for the actor.
void Tick(coop::net::Session& session);

// HOST, at a joiner's world-ready edge: the level to that slot.
void OnPeerWorldReady(int slot);

// CLIENT, a BasementFloodState from the host (the dispatcher has checked the sender and the length).
void OnState(const coop::net::BasementFloodStatePayload& p);
bool WellFormed(const coop::net::BasementFloodStatePayload& p);
void CountDropped();

// Session teardown: the counters go, with one summary line.
void OnDisconnect();

}  // namespace coop::basement_flood_sync
