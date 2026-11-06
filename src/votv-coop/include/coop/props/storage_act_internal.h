// coop/props/storage_act_internal.h -- the host's half of storage_act (storage_act_host.cpp) and the layout half
// (storage_act_layout.cpp), as the client's half (storage_act_client.cpp, which holds the module's entry points) reaches
// them. Not an API: only those files include it.
// Game thread.

#pragma once

#include "coop/net/protocol.h"
#include "coop/props/storage_wire.h"

#include <cstdint>
#include <vector>

namespace coop::net { class Session; }

namespace coop::props::storage_act::host {

void Install(coop::net::Session* session);
void Tick();
void OnDisconnect();
void OnPeerGone(uint8_t slot);
void OnRequestChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);
void OnCommitChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);
// A client's placement-only change (a parsed Family::Layout request): judged, applied and published, or refused and the
// host's own state published again.
void OnLayoutRequest(uint8_t senderSlot, const coop::props::storage_wire::Request& q);

}  // namespace coop::props::storage_act::host

// The free-layout screen's placements: a sort or a drop that has settled is carried from the widgets into the rows,
// and a host publishes it, a client asks the host to (a layout request).
namespace coop::props::storage_act::layout {

void Install(coop::net::Session* session);   // registers the sort and drag-end watches
void Tick();                                 // the pending commit: a sort at once, a drop once its widgets stand still
void OnDisconnect();

}  // namespace coop::props::storage_act::layout

namespace coop::props::storage_act {

// Client: a take or put is waiting for the host's answer. A layout request is held back while it is, since the rows'
// ordinals are only meaningful when no transfer is half made.
bool ActInFlight();
// Client: send one request blob on the request lane (the same sequence the transfers use), and the next txn.
bool SendRequestBlob(const std::vector<uint8_t>& blob);
uint16_t NextTxn();

}  // namespace coop::props::storage_act
