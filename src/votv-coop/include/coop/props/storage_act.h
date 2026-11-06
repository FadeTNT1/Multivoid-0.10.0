// coop/props/storage_act.h -- a client's transfer between its own inventory and a world storage, asked of the host
// before either side changes.
//
// On a client a world storage is the host's: its game may not add, remove, split or merge a row of one. The verbs
// that would are refused at their entry (the script gate): beginDrag of a row the storage holds becomes a take
// intent, an addItemAdvanced the inventory screen runs into a storage becomes a put intent, and every other write
// (removeInventoryItem, transferFromInvToInf, a widget's takeFromStack or addToStack, a hand deposit's add) is
// refused outright, so the storage holds what the host last published. A set the host publishes is applied under
// storage_contents' latch, which the gates let through.
//
// The intent names the storage by its address, the row by its fingerprint (what the item is, not where it lies),
// the amount, the revision the client saw and, for a put, the row itself. The host judges it on its own copy -- the
// sender's world and reach, the lock, the row's presence or the room, the row plan -- and applies it through the
// storage's own verbs: a take's row comes off at the grant and rides the reply, a put's row goes in at the grant.
// The client then makes its own half (the row into, or out of, its player inventory) and closes the act with a
// commit; an abort, a lapse or the sender's departure undoes the host's half. Every change ends in a storage
// publication to every peer, the asker included. A transaction id the host has answered is answered again, never
// run twice; two puts both land, and the last row of a stack goes to the first take. Gameplay layer (principle 7).
// Game thread.

#pragma once

#include "coop/net/protocol.h"
#include "ue_wrap/core/script_gate.h"

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

// The client's gate on transferFromInvToInf, registered with the storage watch that owns the function's row
// (storage_contents).
ue_wrap::script_gate::Verdict GateTransfer(const ue_wrap::script_gate::Call& c);

}  // namespace coop::props::storage_act
