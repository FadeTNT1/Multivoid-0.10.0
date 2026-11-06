// coop/props/storage_act.h -- a client's transfer between its own inventory and a world storage, asked of the host
// before either side changes.
//
// On a client a world storage is the host's, and the verbs that would write one are refused at their entry (the
// script gate): beginDrag of a stored row becomes a take intent, an addItemAdvanced the inventory screen runs into a
// storage a put intent, and every other write (removeInventoryItem, transferFromInvToInf, takeFromStack, addToStack,
// a double-click's use, a hand deposit's add) is refused outright; storage_contents' apply latch is let through.
// The intent names the storage by its address and the row by its fingerprint, with the amount, the revision seen and,
// for a put, the row. The host judges it on its own copy (world, reach, lock, presence or room, row plan) and applies
// its half through the storage's verbs: a take's row comes off at the grant and rides the reply, a put's goes in.
// The client makes its own half in its player inventory and commits; an abort, a lapse or the sender's departure
// undoes a take. A transaction the host answered is answered again, never run twice. Gameplay layer (principle 7).
// Game thread.

#pragma once

#include "coop/net/protocol.h"
#include "coop/props/storage_wire.h"
#include "ue_wrap/core/script_gate.h"

#include <cstddef>
#include <cstdint>

namespace coop::net { class Session; }

namespace coop::props::storage_act {

void Install(coop::net::Session* session);   // registers the client's gates
void Tick();                                 // the client's wait bound and resent commits; the host's hold deadlines
void OnDisconnect();
void OnPeerGone(uint8_t slot);               // host: that slot's holds are undone

// ContainerActRequest and ContainerActCommit chunks (client to host), ContainerActReply chunks (host to client).
void OnRequestChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);
void OnCommitChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);
void OnReplyChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// Host: how many transactions on storage `a` are held, waiting for their client's commit or abort.
size_t HoldsOn(const storage_wire::Address& a);

// The client's gate on transferFromInvToInf, registered with the storage watch that owns the function's row
// (storage_contents).
ue_wrap::script_gate::Verdict GateTransfer(const ue_wrap::script_gate::Call& c);

}  // namespace coop::props::storage_act
