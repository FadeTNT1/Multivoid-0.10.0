// coop/farming/plant_sync.h -- the growing plants (growingPlant_C and every class below it) are the host's.
//
// The host names every live plant by its actor_save key (a level-placed keyless one by its portable identity; a
// runtime keyless one gets a minted key written into it, so the save carries it), and sends each plant's state as a
// PlantState row: at its birth, on an edge (grown, dead, a row or disease change, ripe berries, a move) and on drift at
// most every 5 s. Its death goes as Gone, harvested or not. A joiner gets every row and then the snapshot's end, which
// removes the plants the host did not name.
//
// A client runs none of a plant's own producers (the growth tick, disease rolls and spread, fruit, death, its destroy
// drops, the wild-cane grower, the mushroom spore): those rows are refused at the script-body gate on an active client
// session. It mirrors the host's rows into its plants (adopting its own copy by key or by place, or spawning one) and
// parks their tick. Its harvest, discard, pot planting, tending (watering can, bucket, fertilizer, poo, growinator,
// poog, the dev tools) and medicine go to the host as PlantIntent; the host runs the game's own verb with the sender's
// puppet, and its births ride the prop lanes. A harvested berry the host captured is pocketed by the client once its
// prop arrives. Wire bytes: coop/farming/plant_wire.h. Game thread, all of it.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct BlobChunkPayload;
}  // namespace coop::net

namespace coop::farming {

// Cache the session; register the lane's watches once. Re-entered by the install pump.
void Install(coop::net::Session* session);

// The watches' settle; HOST: the census, queued intents; CLIENT: parked rows, pending pockets; both: the outbound queue.
void Tick(coop::net::Session& session);

// HOST: a joiner's world is up -- every plant's row (snapshot), then the snapshot's end, to that slot. Idempotent.
void OnPeerWorldReady(int slot);

// The routers' entries: one chunk of a PlantIntent (host) or a PlantState (client). The module checks role and sender.
void OnIntentChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);
void OnStateChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// A slot left: its half assemblies and queued intents go.
void OnPeerLeft(uint8_t slot);

// Net disconnect: the client's parked plants tick again; tables and counters go, with one summary line.
void OnDisconnect();

// HOST, for other lanes: `plantActor` was just watered by something this lane does not watch (a sprinkler, a ground
// hose); its row goes at the next census instead of waiting for the drift window. A no-op on a client, for an actor
// that is not a named plant, and with no session. Game thread.
void HostNotePlantWatered(void* plantActor);

}  // namespace coop::farming
