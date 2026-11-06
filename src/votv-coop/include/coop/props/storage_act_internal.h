// coop/props/storage_act_internal.h -- the host's half of storage_act (storage_act_host.cpp), as the client's half
// (storage_act_client.cpp, which holds the module's entry points) reaches it. Not an API: only those files include it.
// Game thread.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::props::storage_act::host {

void Install(coop::net::Session* session);
void Tick();
void OnDisconnect();
void OnPeerGone(uint8_t slot);
void OnRequestChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);
void OnCommitChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

}  // namespace coop::props::storage_act::host
