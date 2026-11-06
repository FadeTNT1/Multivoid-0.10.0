// coop/interactables/tool_action.cpp -- see coop/interactables/tool_action.h.

#include "coop/interactables/tool_action.h"

#include "coop/interactables/pry_sync.h"
#include "coop/interactables/saw_sync.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"

#include <atomic>
#include <cmath>

namespace coop::tool_action {
namespace {

namespace T = coop::net::tool_action;

std::atomic<coop::net::Session*> g_session{nullptr};
uint64_t g_dropped = 0;

bool InRange(float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; }

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    coop::pry_sync::Install(session);
    coop::saw_sync::Install(session);
}

void Tick(coop::net::Session& session) {
    coop::pry_sync::Tick(session);
    coop::saw_sync::Tick(session);
}

bool WellFormed(const coop::net::ToolActionPayload& p) {
    if (p.kind != T::kPry && p.kind != T::kSaw) return false;
    if (p.op < T::kBegin || p.op > T::kRefuse) return false;
    if (p.result > T::kLastResult || (p.op != T::kEnd && p.result != 0)) return false;
    if ((p.flags & ~T::kKnownFlags) != 0 || p.reserved[0] != 0 || p.reserved[1] != 0) return false;
    if (p.target.len == 0 || p.target.len > sizeof(p.target.data)) return false;
    for (float v : p.pivot)
        if (!InRange(v, -1.0e7f, 1.0e7f)) return false;
    if (!InRange(p.alpha, -0.5f, 1.5f) || !InRange(p.pitch, -360.f, 360.f) || !InRange(p.angle, -360.f, 360.f))
        return false;
    // A saw's begin carries its cut's whole time in `stress`; everything else is the strain, 0..1.5.
    const float stressHi = (p.kind == T::kSaw && p.op == T::kBegin) ? 36000.f : 1.5f;
    if (!InRange(p.stress, -0.5f, stressHi)) return false;
    return true;
}

void CountDropped() { ++g_dropped; }

void OnIntent(coop::net::Session& session, const coop::net::ToolActionPayload& p, uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host || senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    if (p.kind == T::kPry) coop::pry_sync::OnIntent(session, p, senderSlot);
    else coop::saw_sync::OnIntent(session, p, senderSlot);
}

void OnState(coop::net::Session& session, const coop::net::ToolActionPayload& p) {
    if (session.role() != coop::net::Role::Client) return;
    if (p.kind == T::kPry) coop::pry_sync::OnState(session, p);
    else coop::saw_sync::OnState(session, p);
}

void ConnectReplayForSlot(int slot) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Host) return;
    coop::pry_sync::ConnectReplayForSlot(*s, slot);
    coop::saw_sync::ConnectReplayForSlot(*s, slot);
}

void OnPeerLeft(uint8_t slot) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    coop::pry_sync::OnPeerLeft(*s, slot);
    coop::saw_sync::OnPeerLeft(*s, slot);
}

void OnDisconnect() {
    coop::pry_sync::OnDisconnect();
    coop::saw_sync::OnDisconnect();
    if (g_dropped) UE_LOGI("[TOOL] session end -- %llu ill-formed tool actions dropped", static_cast<unsigned long long>(g_dropped));
    g_dropped = 0;
}

bool Send(coop::net::Session& session, const coop::net::ToolActionPayload& p, int skipSlot) {
    const coop::net::ReliableKind kind = session.role() == coop::net::Role::Host
                                             ? coop::net::ReliableKind::ToolActionState
                                             : coop::net::ReliableKind::ToolActionIntent;
    return session.SendReliable(kind, &p, static_cast<int>(sizeof(p)), skipSlot);
}

bool SendToSlot(coop::net::Session& session, int slot, const coop::net::ToolActionPayload& p) {
    if (session.role() != coop::net::Role::Host) return false;
    return session.SendReliableToSlot(slot, coop::net::ReliableKind::ToolActionState, &p, static_cast<int>(sizeof(p)));
}

}  // namespace coop::tool_action
