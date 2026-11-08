// coop/props/dig_sync.h -- the holes a player digs and what is put in them are the host's. docs/props.md, "Holes and
// buried items", has the why; docs/coop-sync-doctrine.md records the doctrine this narrows.
//
// THE FAMILY: a hole (dirthole_C, a prop_C that mints no key, so no prop lane names it), the mound a buried item sits
// in (dirthole_item_C, an actor_save_C that holds the item's whole save record in `betterData`) and the plant a seed
// becomes (growingPlant_C). The host names each by a number it mints, watches liveness, and sends DigState (class,
// place, material or record, or gone). The underground loot mounds (ticker_susHoleSpawner, a `betterData` with no
// class) are NOT in the family and stay per-peer, as garbage_sync records.
//
// A CLIENT'S VERBS are intents the host runs natively (DigIntent): a dig (the shovel's event_dighole still runs on the
// client for its crater, sounds and animation; the hole it spawns is read at its BeginPlay and destroyed, and the host
// spawns the one hole every peer then mirrors), a burial or planting (the hole's playerUsedOn is refused; the held
// item's own save record goes with the intent and the host rebuilds the item and runs processBuriedItem) and a fill
// (a dirt ball used on a hole, or thrown into it: dig_fill). A mirror hole's BeginPlay (which digs a mound up and attaches the hole) and its fill
// overlap are refused on a client: the host runs them and the results arrive as DigState and the prop lane.
//
// LATE JOIN: the save carries the holes and mounds natively; the ready edge sends every row and an end marker, and a
// joiner adopts its own native actor at each row's place (class and place within a few centimetres) or spawns one.
// Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct BlobChunkPayload;
}  // namespace coop::net

namespace coop::dig_sync {

// Cache the session and register the watches once. Re-entered by the install pump.
void Install(coop::net::Session* session);

// The watches' settle; HOST: the intents run, the census published, the queue sent; CLIENT: the captured digs sent,
// the parked rows retried.
void Tick(coop::net::Session& session);

// HOST: a joiner's world is up: every row to it, then the snapshot's end.
void OnPeerWorldReady(int slot);

// HOST: one chunk of a client's intent. CLIENT: one chunk of the host's state.
void OnIntentChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);
void OnStateChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// Net disconnect: the tables and queues go, with one summary line. The holes stay where they are.
void OnDisconnect();

}  // namespace coop::dig_sync
