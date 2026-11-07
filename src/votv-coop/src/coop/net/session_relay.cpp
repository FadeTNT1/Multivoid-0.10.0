// coop/net/session_relay.cpp -- host-relay topology fan-out.

// The "host as relay hub" subsystem. In the star topology a packet from client A reaches only the
// host, so these two methods forward A's packets to the OTHER clients and peers can see each other.
// MTA has the same shape: its server relays puresync and RPC as they arrive.
//
// Both are Session member functions, declared in coop/net/session.h and defined here, and they are
// called only from Session::HandleMessage, on the net thread, host role only.
//
// The header rewrite is common to both. The forwarded datagram's senderEpoch is replaced with the
// HOST's own epoch, because from the receiving client's point of view the packet rides ITS host
// connection, whose epoch it latched, so the relayed copy must carry the host epoch to pass that
// connection-keyed latch. senderSlot is set to the true ORIGIN connection slot, so the receiver
// routes the payload to the right peer's puppet, and originContext to the host's number for that
// slot's current occupancy, so a receiver can tell a rejoined occupant's packets, numbered from zero
// again, from the last occupant's late ones (coop/net/origin_context). seq and body are preserved.

#include "coop/net/session.h"

#include "coop/player/pocket_travel.h"       // the pocket rule: who stands in which level
#include "coop/player/pocket_travel_host.h"  // ... and who is parked in a pocket
#include "ue_wrap/core/log.h"
#include "session_lanes.h"  // Lane, LaneForKind (co-located private header)

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#pragma warning(pop)

#include <cstdint>
#include <cstring>

namespace coop::net {

void Session::RelayUnreliableToOtherClients(int originSlot, const void* data, int len) {
    // Forward an unreliable datagram -- a pose, a prop pose, a voice frame -- that the host just
    // received from `originSlot` to every OTHER connected client, with the origin's seq, so a receiver
    // judges order per origin (coop/net/stream_slot.h). MTA's server re-sends a relayed puresync as a
    // packet of its own, so a client judges the server's order
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/net/CSimPlayerManager.cpp:229-249).
    if (cfg_.role != Role::Host) return;
    if (len < static_cast<int>(sizeof(PacketHeader)) || len > kMaxPacketBytes) return;
    auto* sockets = SteamNetworkingSockets();
    if (!sockets) return;
    uint8_t buf[kMaxPacketBytes];
    std::memcpy(buf, data, static_cast<size_t>(len));
    auto* h = reinterpret_cast<PacketHeader*>(buf);
    h->senderEpoch = ownEpoch_;
    h->senderSlot = static_cast<uint8_t>(originSlot);
    h->originContext = originContext_.Stamp(originSlot);
    coop::net::WriteStateTimeMs24(*h, 0);  // the origin's state time is not the relayer's -- scrub (no client-side reader)
    // The pocket rule (coop/player/pocket_travel, docs/pocket-dimensions.md): a player's body and
    // voice reach another player only when both stand in the same level. Two clients in the same
    // pocket are both not world-ready (they have no main world), so for them the pre-world gate is
    // carved open, for the body and voice kinds only: a held prop's pose or a desk cursor names a
    // thing in a world, and each pocket copy is its client's own. Read through the location table's
    // atomics: this is the net thread.
    namespace PT = coop::player::pocket_travel;
    namespace PTH = coop::player::pocket_travel_host;
    const auto type = static_cast<MsgType>(h->type);
    const bool bodyOrVoice = type == MsgType::PoseSnapshot || type == MsgType::RagdollPose ||
                             type == MsgType::HandPose || type == MsgType::VoiceFrame;
    const auto origin = static_cast<uint8_t>(originSlot);
    const bool originParked = PTH::IsSlotParked(origin);
    for (int i = 1; i < kMaxPeers; ++i) {
        if (i == originSlot) continue;
        const uint32_t hConn = peerConns_[i].load();
        if (hConn == 0) continue;
        const auto to = static_cast<uint8_t>(i);
        if (!PT::SameLocation(origin, to)) continue;
        // Pre-world gate: no pose relays to a joiner still at the menu -- except between two
        // players parked in the same pocket (above).
        if (!IsSlotWorldReady(i) &&
            !(bodyOrVoice && originParked && PTH::IsSlotParked(to) && PT::IsPocket(PT::LocationOf(to))))
            continue;
        const EResult rc = sockets->SendMessageToConnection(
            hConn, buf, len, k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
        if (rc == k_EResultOK) {
            net_stats::AddSent(static_cast<uint32_t>(len));
            // The relay fills OTHER peers' send buffers, so it is a producer the headroom
            // estimate has to see, exactly like the direct fan-out beside it -- and for the same
            // reason it is unreliable load the send-rate law must count against the rate it bounds.
            admission_.NoteHanded(i, len);
            rateControl_.NoteUnreliableQueued(i, len);
        }
    }
}

void Session::RelayReliableToOtherClients(int originSlot, ReliableKind kind,
                                          const void* data, int len) {
    // The reliable sibling of the unreliable relay: forward a peer-originated gameplay reliable to
    // every OTHER client, on the reliable channel and the kind's priority lane. HandleMessage has
    // already filtered to relayable kinds through IsClientRelayableReliableKind.
    //
    // DoS note: a flooding client makes the host fan out one message to every other peer. Per-peer
    // relay rate limiting is not built. What does bound it is downstream and indirect: a receiver
    // stops draining at kReliableInboxSoftPause (session.cpp) while GNS buffers losslessly
    // beneath, so a flood turns into back-pressure on the sender rather than unbounded memory.
    if (cfg_.role != Role::Host) return;
    if (len < static_cast<int>(sizeof(PacketHeader)) || len > kMaxPacketBytes) return;
    // GNS availability guard (the backlog's send helper re-checks per attempt).
    if (!SteamNetworkingSockets() || !SteamNetworkingUtils()) return;
    const int laneIdx = static_cast<int>(LaneForKind(kind));
    for (int i = 1; i < kMaxPeers; ++i) {
        if (i == originSlot) continue;
        const uint32_t hConn = peerConns_[i].load();
        if (hConn == 0) continue;
        // Pre-world gate: relayed gameplay reliables (all world-mutating
        // by the relay whitelist's nature) skip a joiner still at the menu -- its
        // world-ready replay re-derives the state. Seeds arc: IsRelayEligible is
        // the NET-THREAD view of the same world-ready event (stamped at the
        // ClientWorldReady receipt) -- without it, a peer reliable received in the
        // receipt->GT-flip gap was skipped here AND missed by the ready-edge seed.
        if (!IsSlotWorldReady(i) && !IsRelayEligible(i, hConn) &&
            !IsPreWorldSendableKind(kind)) continue;
        // The pocket rule: a slot away in a pocket dimension gets no relayed world kind, whatever
        // its eligibility says (its stamp is cleared at its departure; this holds the gap).
        if (coop::player::pocket_travel_host::IsSlotIsolated(static_cast<uint8_t>(i)) &&
            !IsAwaySendableKind(kind)) continue;
        // The relay rides the same delivery guarantee as every other reliable send. Before the
        // backlog existed, a refusal here was the worst silent-loss surface in the mod -- the
        // message was dropped without even a warning, so the field's refusal count under-reported
        // for any session past two peers. Rewrite the header on a stack copy and hand the final
        // wire bytes to the backlog.
        uint8_t wire[kMaxPacketBytes];
        std::memcpy(wire, data, static_cast<size_t>(len));
        auto* h = reinterpret_cast<PacketHeader*>(wire);
        h->senderEpoch = ownEpoch_;
        h->senderSlot = static_cast<uint8_t>(originSlot);
        h->originContext = originContext_.Stamp(originSlot);
        coop::net::WriteStateTimeMs24(*h, 0);  // the origin's state time is not the relayer's -- scrub (no client-side reader)
        if (backlog_.SendOrQueue(i, laneIdx, hConn, wire, len, admission_) == SendOutcome::Streamed)
            rateControl_.NoteReliableQueued(i, len);
    }
}

}  // namespace coop::net
