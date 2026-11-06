// coop/dispatch/event_dispatch_container_act.cpp -- a client's transfer with a world storage: ContainerActRequest and
// ContainerActCommit (client to host) and ContainerActReply (host to client), each a chunked blob handed to
// coop::props::storage_act. Each case validates at the trust boundary. The switch below is the family's membership
// declaration; nothing re-lists it. Family contract per coop/dispatch/event_dispatch.h: returns true iff msg.kind is
// in this family.

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/props/storage_act.h"  // the client's gates and answer, the host's judge and holds

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::event_feed {
namespace {

// A chunk from a client slot, or false (said once per kind and reason, never per message: a sender cannot make the
// router's log the cost).
bool ClientChunk(const net::Session::ReliableMessage& msg, net::BlobChunkPayload& chunk, uint8_t& slot) {
    if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers || msg.payloadLen < sizeof(chunk)) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGW("event_feed: a container act chunk from slot %d (%zu B) dropped -- bad sender or short payload",
                    msg.senderPeerSlot, static_cast<size_t>(msg.payloadLen));
        }
        return false;
    }
    std::memcpy(&chunk, msg.payload, sizeof(chunk));
    slot = static_cast<uint8_t>(msg.senderPeerSlot);
    return true;
}

}  // namespace

bool HandleContainerActEvent(net::Session& session,
                             const net::Session::ReliableMessage& msg) {
    switch (msg.kind) {
    case net::ReliableKind::ContainerActRequest: {
        // CLIENT->HOST: a client asks to take a row out of a world storage, or to put one in
        // (coop::props::storage_act). Never relayed.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: ContainerActRequest received on a client -- dropping");
            break;
        }
        net::BlobChunkPayload chunk{};
        uint8_t slot = 0;
        if (ClientChunk(msg, chunk, slot)) coop::props::storage_act::OnRequestChunk(chunk, slot);
        break;
    }
    case net::ReliableKind::ContainerActCommit: {
        // CLIENT->HOST: the client's half of a granted act ran or did not; the commit ends the host's hold. Never
        // relayed.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: ContainerActCommit received on a client -- dropping");
            break;
        }
        net::BlobChunkPayload chunk{};
        uint8_t slot = 0;
        if (ClientChunk(msg, chunk, slot)) coop::props::storage_act::OnCommitChunk(chunk, slot);
        break;
    }
    case net::ReliableKind::ContainerActReply: {
        // HOST->CLIENT: the host's answer to this client's act; a granted take carries the row. Never relayed.
        if (session.role() != net::Role::Client || msg.senderPeerSlot != 0 ||
            msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: ContainerActReply dropped (role, sender slot %d or length %zu)", msg.senderPeerSlot,
                    static_cast<size_t>(msg.payloadLen));
            break;
        }
        net::BlobChunkPayload chunk{};
        std::memcpy(&chunk, msg.payload, sizeof(chunk));
        coop::props::storage_act::OnReplyChunk(chunk, 0);
        break;
    }
    default:
        return false;  // not a container-act kind -> event_feed tries the next family
    }
    return true;  // a container-act kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
