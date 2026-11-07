// coop/net/session_voice.cpp -- voice-frame send/receive for Session.
//
// Owns the unreliable voice STREAM (MsgType::VoiceFrame): the game-thread fan-out send
// (SendVoiceFrame -- GNS send APIs are thread-safe, the SendReliable calling convention), the
// receive-side inbox store (StoreVoiceFrame, net thread, called from HandleMessage's case) and
// the game-thread batch drain (DrainVoiceFrames). A per-sender FIFO STREAM, not the newest-wins
// pose model: every arrival queues, and ordering and loss live in the per-payload voice seq at
// the jitter buffer (coop/voice/voice_playback). The inbox is per-slot fixed rings: no
// net-thread heap alloc, per-sender overflow fairness. Mutex discipline: voiceInboxMutex_
// only.

#include "coop/net/session.h"
#include "coop/player/pocket_travel.h"  // the pocket rule: voice stays inside one level

#include "ue_wrap/core/log.h"

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include <cstring>
#include <mutex>

namespace coop::net {

bool Session::SendVoiceFrame(const VoiceFramePayload& frame) {
    if (frame.opusLen > kVoiceMaxOpusBytes) return false;
    const int bodyLen = kVoiceFrameHeadBytes + frame.opusLen;
    const int total = static_cast<int>(sizeof(PacketHeader)) + bodyLen;
    const uint32_t seq = sendSeq_.fetch_add(1);

    auto* sockets = SteamNetworkingSockets();
    auto* utils = SteamNetworkingUtils();

    bool anySuccess = false;
    for (int i = 0; i < kMaxPeers; ++i) {
        const uint32_t hConn = peerConns_[i].load();
        if (hConn == 0) continue;
        if (!IsSlotWorldReady(i)) continue;  // a menu-mode joiner has no listener yet
        // The pocket rule: this machine's player speaks to the players in its own level. A slot
        // away in a pocket is never world-ready, so the gate above holds it already; asked by name
        // so the rule does not rest on that.
        if (!coop::player::pocket_travel::SameLocation(0, static_cast<uint8_t>(i))) continue;
        SteamNetworkingMessage_t* msg = utils->AllocateMessage(total);
        if (!msg) continue;
        auto* buf = static_cast<uint8_t*>(msg->m_pData);
        auto* hdr = reinterpret_cast<PacketHeader*>(buf);
        WriteHeader(*hdr, MsgType::VoiceFrame, seq, ownEpoch_);
        std::memcpy(buf + sizeof(PacketHeader), &frame, bodyLen);
        msg->m_conn = hConn;
        msg->m_nFlags = k_nSteamNetworkingSend_UnreliableNoDelay;
        int64 outMsgNum = 0;
        sockets->SendMessages(1, &msg, &outMsgNum, /*bDeleteFailedMessages*/true);
        if (outMsgNum >= 0) {
            net_stats::AddSent(static_cast<uint32_t>(total));
            // Voice is the one unreliable stream sent off the net thread, so it is also the only
            // one that can fill a connection between two anchors; counting it keeps the headroom
            // estimate from reading low. The send-rate counter beside it is an atomic for the same
            // reason -- this is the one unreliable producer that is not the net thread.
            admission_.NoteHanded(i, total);
            rateControl_.NoteUnreliableQueued(i, total);
            anySuccess = true;
        }
    }
    return anySuccess;
}

void Session::StoreVoiceFrame(int routeSlot, int peerSlot, const void* data, int len) {
    const int minLen = static_cast<int>(sizeof(PacketHeader)) + kVoiceFrameHeadBytes;
    if (len < minLen) return;
    if (routeSlot < 0 || routeSlot >= kMaxPeers) return;  // ring index safety
    const int bodyLen = len - static_cast<int>(sizeof(PacketHeader));
    VoiceFrameMsg vm{};
    vm.senderSlot = static_cast<int8_t>(routeSlot);
    const int copyLen = bodyLen > static_cast<int>(sizeof(vm.frame))
                            ? static_cast<int>(sizeof(vm.frame)) : bodyLen;
    std::memcpy(&vm.frame, static_cast<const uint8_t*>(data) + sizeof(PacketHeader), copyLen);
    if (vm.frame.opusLen > kVoiceMaxOpusBytes ||
        kVoiceFrameHeadBytes + vm.frame.opusLen > bodyLen)
        return;  // truncated/poisoned datagram
    // The pocket rule (coop/player/pocket_travel): the host stands in the main world and hears only
    // the players there; a client's voice from a pocket is still relayed to a client in the same
    // pocket (RelayUnreliableToOtherClients judges the location). Net thread: the table's atomics.
    const bool hostHears = cfg_.role != Role::Host || routeSlot == 0 ||
        coop::player::pocket_travel::SameLocation(static_cast<uint8_t>(routeSlot), 0);
    if (hostHears) {
        std::lock_guard<std::mutex> lk(voiceInboxMutex_);
        VoiceSlotRing& r = voiceRings_[routeSlot];
        if (r.tail - r.head >= static_cast<uint32_t>(kVoiceRingPerSlot))
            ++r.head;  // full: shed THIS sender's oldest frame only
        r.ring[r.tail % kVoiceRingPerSlot] = vm;
        ++r.tail;
    }
    // Host relay: every other client hears this speaker (receiver-side
    // attenuation silences out-of-range audio; see the MsgType doc).
    if (cfg_.role == Role::Host) {
        RelayUnreliableToOtherClients(peerSlot, data, len);
    }
}

int Session::DrainVoiceFrames(VoiceFrameMsg* out, int maxCount) {
    int n = 0;
    std::lock_guard<std::mutex> lk(voiceInboxMutex_);
    for (int slot = 0; slot < kMaxPeers && n < maxCount; ++slot) {
        VoiceSlotRing& r = voiceRings_[slot];
        while (r.head != r.tail && n < maxCount) {
            out[n++] = r.ring[r.head % kVoiceRingPerSlot];
            ++r.head;
        }
    }
    return n;
}

}  // namespace coop::net
