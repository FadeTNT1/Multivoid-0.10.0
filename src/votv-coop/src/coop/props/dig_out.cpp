// coop/props/dig_out.cpp -- see dig_out.h.

#include "dig_out.h"

#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "ue_wrap/core/log.h"

#include <deque>
#include <utility>

namespace coop::dig_out {
namespace {

constexpr size_t kMaxQueuedOut = 1024;   // a joiner's whole snapshot (every row and its end) fits

struct Out {
    int                  slot;   // -1: every client (state); a client's intents go to the host
    std::vector<uint8_t> bytes;
};
std::deque<Out> g_out;
uint32_t g_sendSeq = 1;
bool g_fullSaid = false;

}  // namespace

void Enqueue(int slot, std::vector<uint8_t>&& bytes) {
    if (g_out.size() >= kMaxQueuedOut) {
        if (!g_fullSaid) {
            g_fullSaid = true;
            UE_LOGW("[DIG] the outbound queue is full (%zu) -- a message is dropped", g_out.size());
        }
        return;
    }
    g_out.push_back(Out{slot, std::move(bytes)});
}

void Drain(coop::net::Session& s) {
    if (!s.running() || !s.connected()) return;
    const bool host = s.role() == coop::net::Role::Host;
    const auto kind = host ? coop::net::ReliableKind::DigState : coop::net::ReliableKind::DigIntent;
    for (int n = 0; n < 16 && !g_out.empty(); ++n) {
        const Out& o = g_out.front();
        // HOST: a peer takes the dig lane only once its world is up, and its world-ready snapshot then carries every
        // row as it stands. A broadcast no ready peer can take, or a row for a slot no longer ready (it left), is
        // already covered or moot; held at the head it refused every tick and blocked the joiner's snapshot.
        if (host && (o.slot < 0 ? !s.AnyWorldReadyPeer() : !s.IsSlotWorldReady(o.slot))) {
            g_out.pop_front();
            continue;
        }
        const bool ok = (host && o.slot >= 0)
                            ? coop::blob_chunks::SendBlobToSlot(&s, o.slot, kind, g_sendSeq, o.bytes)
                            : coop::blob_chunks::SendBlob(&s, kind, g_sendSeq, o.bytes);
        if (!ok) return;   // the blob is all or nothing: the whole of it again next tick, under a fresh number
        ++g_sendSeq;
        g_out.pop_front();
    }
}

void Clear() {
    g_out.clear();
    g_fullSaid = false;
}

}  // namespace coop::dig_out
