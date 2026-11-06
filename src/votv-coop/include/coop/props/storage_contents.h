// coop/props/storage_contents.h -- a world storage's contents, published by the host to every peer.
//
// The host owns every world storage (ue_wrap/actors/storage_inventory): a container prop, the delivery drone. Any
// change its own game makes to one -- a row added, removed or split, a sack filled from the drone, a sale that
// empties it, a client's transfer the host applied -- runs the component's calculateVolumes or
// transferFromInvToInf, which this lane watches; the storage is marked and, at the next drain, its rows are read,
// and when they differ from what was last sent the revision rises and the whole set goes to every world-ready peer,
// the client who asked for the change included. The drone is also read once a second, for a write the watches do
// not see. A joiner receives every storage at its world-ready edge, the empty ones too, since its own save may hold
// rows the host's world no longer does.
//
// A client applies a set only when its revision is newer than the last it applied for that storage, so a set that
// overtook another never brings back an older state. It applies under a latch (InApply) that storage_act's gates read,
// and a set for a storage whose actor is not here yet waits (bounded) for it. Gameplay layer (principle 7).
// Game thread.

#pragma once

#include "coop/net/protocol.h"
#include "coop/props/storage_wire.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::props::storage_contents {

void Install(coop::net::Session* session);   // registers the host's watches
void Tick();                                 // the host's drain and drone read; a client's waiting sets
void OnDisconnect();

// ContainerContents chunks: the host's pages. A client accepts slot 0 only; the host refuses every one.
void OnContentsChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// Host: every world storage, to one slot that has just become world-ready.
void QueueConnectBroadcastForSlot(int peerSlot);

// Host: a storage changed by this peer's own hand (storage_act's apply); published at the next drain.
void MarkDirty(void* component);

// Host: the storage's rows go out again under a new revision, even though they are unchanged -- a client's layout was
// refused, and its local layout is redrawn from this.
void Resync(const storage_wire::Address& a);

// The wire address of a world storage, and back. False / null when it is not one, or names nothing here.
bool AddressOf(void* component, storage_wire::Address& out);
void* Resolve(const storage_wire::Address& a);

// The revision this peer last published (host) or applied (client) for the storage; 0 when none.
uint32_t RevisionOf(const storage_wire::Address& a);

// Client: true while a host set is being written into a storage here.
bool InApply();

// A storage's address for a log line: "prop 1234" or "drone". Points into a static buffer; one line at a time.
const char* Describe(const storage_wire::Address& a);

}  // namespace coop::props::storage_contents
