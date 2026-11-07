// coop/items/irrigation_sync.h -- the irrigation set: the placed faucet (faucet_C), the ground hose (groundhose_C)
// and the sprinkler they feed (prop_sprinkler_C, a prop). docs/deployables.md, "Irrigation", has the why.
//
// The faucet and the hose are actor_save_C, so no prop lane carries them; both are the host's, named by their key.
// A client's placement runs natively; the birth is caught inside the held item's own left-button body and its record
// (getData) goes to the host as IrrigationCommit, a hose's once it settles. The host judges and spawns its copy under
// the same key, linked and finished, and polls its faucets and hoses: IrrigationState carries a birth or a change
// (the record, links rewritten to the live ones) or an end (the key). A client's faucet take is cancelled and run on
// the host; a hose's pickup and a sprinkler's detach run here and are told to the host. The faucet's on/off is the
// appliance lane's. Late join: the save carries both; the world-ready edge sends every one, bracketed, and the joiner
// destroys any the host did not name. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct BlobChunkPayload;
}  // namespace coop::net

namespace coop::irrigation_sync {

void Install(coop::net::Session* session);
// The watches' settle; CLIENT: the births to commit; HOST: the poll and the sweep.
void Tick(coop::net::Session& session);
// HOST: a joiner's world is up: every placed faucet and hose to it, bracketed.
void OnPeerWorldReady(int slot);
// HOST: a client's verb, chunked. CLIENT: the host's state, chunked.
void OnCommitChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);
void OnStateChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);
void OnPeerLeftSlot(int slot);
void OnDisconnect();

// The watering seam the farming lane reads. growingplant_C.water has three writers in the irrigation set: a
// sprinkler's doWater (every 0.1 s, the plants in its wateringRadius, +0.3 * (1 - d / r) while below 600), a held
// hose's spray (groundhose_C.ReceiveTick, +100/s at its landing point while below 600) and the watering can. This lane
// cancels doWater on a connected client, so a sprinkler waters the HOST's plants only and a client's plants take their
// water from the farming lane's host state. True where doWater runs: the host, or no session.
bool SprinklerWatersHere();

}  // namespace coop::irrigation_sync
