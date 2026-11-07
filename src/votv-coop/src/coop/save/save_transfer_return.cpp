// coop/save/save_transfer_return.cpp -- the save transfer run a second time for an admitted slot:
// a pocket trip's way home (coop/player/pocket_travel_host, docs/pocket-dimensions.md). The host
// serves the returning slot's request as at its join; the client re-arms its half and asks again.
// The transfer itself is save_transfer.cpp's, unchanged.

#include "coop/save/save_transfer.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/pocket_travel_host.h"  // IsSlotReturning: the one slot a re-arm serves
#include "coop/props/save_identity_bind.h"   // the client's eid-range bind, started over
#include "save_transfer_internal.h"          // co-located: the request latch
#include "ue_wrap/core/log.h"

#include <atomic>
#include <cstdint>

namespace coop::save_transfer {
namespace {
std::atomic<coop::net::Session*> g_session{nullptr};
}  // namespace

void detail::InstallReturn(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void HostArmReturnForSlot(int peerSlot) {
    if (peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return;
    if (!coop::player::pocket_travel_host::IsSlotReturning(static_cast<uint8_t>(peerSlot))) {
        UE_LOGW("save_transfer: slot %d -- a return re-arm for a slot that is not on its way home from a "
                "pocket; refused (a second request stays the protocol error MV-H33 ends)", peerSlot);
        return;
    }
    // Whatever its join left: a stream, a capture wait, the seed snapshots and the window baselines,
    // whose settle phase would otherwise keep the new window's late flush shut. Then the latch.
    CancelForSlot(peerSlot);
    detail::ClearRequestLatchForReturn(peerSlot);
    UE_LOGI("save_transfer: slot %d -- re-armed for its way home from a pocket: its next request is served "
            "as at a join (a fresh live capture, seeds and baselines)", peerSlot);
}

bool ClientArmForReturn() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected() || s->role() != coop::net::Role::Client) {
        UE_LOGW("save_transfer: client return re-arm refused -- no client session is up");
        return false;
    }
    UE_LOGI("save_transfer: client re-arming for the way home from a pocket");
    // The join's own two steps: the reset and arm (the latches, the state, the progress, the received
    // blob), then the request, which the connect edge will not send again. The identity map the first
    // load bound against was that world's; the return's stream brings its own.
    ClientArm();
    coop::save_identity_bind::OnDisconnect();
    ClientNoteConnected();
    return true;
}

}  // namespace coop::save_transfer
