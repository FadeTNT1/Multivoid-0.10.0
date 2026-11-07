// coop/interactables/piece_break_fx.cpp -- see coop/interactables/piece_break_fx.h.

#include "coop/interactables/piece_break_fx.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/asologo_piece.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace coop::piece_break_fx {
namespace {

namespace EL = coop::element;
namespace R  = ue_wrap::reflection;
namespace AP = ue_wrap::asologo_piece;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowBreak, "piece_break_fx", L"prop_asologoPiece_C", L"break", Declared);
constexpr int kTag = 0x50424658;  // 'PBFX'

constexpr uint64_t kSameBreakMs = 3000;   // a second cue for one piece inside this is a repeat, not a break
constexpr uint64_t kPendingMs = 4000;     // a cue whose copy has not resolved is retried for this long
constexpr size_t   kPendingMax = 8;
constexpr size_t   kRecentMax = 16;

std::atomic<coop::net::Session*> g_session{nullptr};

enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg = Reg::Pending;
bool g_settled = false;

int g_applying = 0;   // this lane's own break() on a copy: its watch notes nothing and sends nothing

struct Recent {
    uint32_t eid = 0;
    uint64_t ms = 0;
};
// HOST: the pieces whose break went out. CLIENT: the pieces whose break() ran here, natively or from a cue.
std::vector<Recent> g_recent;

struct Waiting {
    uint32_t eid = 0;
    uint64_t dueMs = 0;   // give up at this time
};
std::vector<Waiting> g_waiting;

uint32_t g_sent = 0, g_played = 0, g_repeats = 0, g_ranHere = 0, g_lost = 0, g_noEid = 0, g_late = 0;

coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }
uint64_t NowMs() { return coop::net::NowMs(); }

bool SeenRecently(uint32_t eid, uint64_t now) {
    for (const Recent& r : g_recent)
        if (r.eid == eid && now - r.ms < kSameBreakMs) return true;
    return false;
}

void Remember(uint32_t eid, uint64_t now) {
    for (Recent& r : g_recent)
        if (r.eid == eid) { r.ms = now; return; }
    if (g_recent.size() >= kRecentMax) g_recent.erase(g_recent.begin());
    g_recent.push_back(Recent{eid, now});
}

// The piece's break() ran, here. A host sends it to every client; a client notes it, so the host's cue for the same
// piece does not play it again. What this lane runs itself (g_applying) is neither.
void OnBreakPost(const sg::Call& c) {
    coop::net::Session* s = LoadSession();
    if (g_applying > 0 || !s || !s->connected() || !AP::IsPiece(c.object)) return;
    const EL::ElementId eid = EL::Registry::Get().EidForActor(c.object);
    const bool host = s->role() == coop::net::Role::Host;
    if (eid == EL::kInvalidId) {
        if (host && ++g_noEid <= 3)
            UE_LOGW("[PIECE-BREAK] host: %ls broke but has no element id -- the other peers see it without the effect",
                    R::ClassNameOf(c.object).c_str());
        return;
    }
    const uint64_t now = NowMs();
    if (host && SeenRecently(eid, now)) {
        ++g_repeats;
        return;
    }
    Remember(eid, now);
    if (!host) {
        ++g_ranHere;
        return;
    }
    coop::net::PieceBreakFxPayload p{};
    p.targetEid = eid;
    if (!s->SendReliable(coop::net::ReliableKind::PieceBreakFx, &p, static_cast<int>(sizeof(p)))) return;
    ++g_sent;
    UE_LOGI("[PIECE-BREAK] host: %ls eid=%u broke -- the cue goes to every client (#%u)",
            R::ClassNameOf(c.object).c_str(), static_cast<unsigned>(eid), g_sent);
}

void RegisterWatch() {
    if (g_reg != Reg::Pending) return;
    g_reg = sg::WatchName(kRowBreak, kTag, nullptr, &OnBreakPost) ? Reg::Registered : Reg::Refused;
}

void Settle() {
    if (g_settled || g_reg == Reg::Pending) return;   // nothing is judged before the watch is registered
    if (g_reg == Reg::Registered && sg::NameWatchLive(kRowBreak, kTag)) {
        g_settled = true;
        UE_LOGI("[PIECE-BREAK] the asologo piece break() watch is live");
    } else if (g_reg == Reg::Refused || sg::NameWatchSettled(kRowBreak, kTag)) {
        g_settled = true;
        UE_LOGE("[PIECE-BREAK] the asologo piece break() watch is dead -- a pried radiator's effects are seen only on "
                "the host");
    }
}

// Run break() on the copy `eid` names. True when the cue is done with: played, or this copy cannot play it.
bool Play(uint32_t eid, bool retried) {
    void* actor = EL::LivePropActor(static_cast<EL::ElementId>(eid));
    if (!actor) return false;
    if (!AP::IsPiece(actor)) {
        UE_LOGW("[PIECE-BREAK] client: eid=%u is %ls here, not a pryable piece -- the cue is dropped",
                static_cast<unsigned>(eid), R::ClassNameOf(actor).c_str());
        ++g_lost;
        return true;
    }
    ++g_applying;
    const bool ok = AP::CallBreak(actor);
    --g_applying;
    Remember(eid, NowMs());
    if (!ok) {
        UE_LOGW("[PIECE-BREAK] client: break() on %ls eid=%u did not dispatch -- no effect here",
                R::ClassNameOf(actor).c_str(), static_cast<unsigned>(eid));
        ++g_lost;
        return true;
    }
    ++g_played;
    if (retried) ++g_late;
    UE_LOGI("[PIECE-BREAK] client: %ls eid=%u break() played%s (#%u)", R::ClassNameOf(actor).c_str(),
            static_cast<unsigned>(eid), retried ? " after a wait for its copy" : "", g_played);
    return true;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (session) RegisterWatch();
}

void Tick() {
    if (!g_settled) {
        sg::ResolvePendingNames();
        Settle();
    }
    if (g_waiting.empty()) return;
    const uint64_t now = NowMs();
    for (size_t i = 0; i < g_waiting.size();) {
        const Waiting w = g_waiting[i];
        if (Play(w.eid, true)) {
            g_waiting.erase(g_waiting.begin() + static_cast<long long>(i));
        } else if (now >= w.dueMs) {
            UE_LOGW("[PIECE-BREAK] client: no prop answers eid=%u after %llu ms -- the cue is dropped",
                    static_cast<unsigned>(w.eid), static_cast<unsigned long long>(kPendingMs));
            ++g_lost;
            g_waiting.erase(g_waiting.begin() + static_cast<long long>(i));
        } else {
            ++i;
        }
    }
}

bool WellFormed(const coop::net::PieceBreakFxPayload& p, int len) {
    return len >= static_cast<int>(sizeof(p)) && p.targetEid != 0 && p.targetEid != EL::kInvalidId && p.reserved == 0;
}

void OnFx(const coop::net::PieceBreakFxPayload& p) {
    coop::net::Session* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Client) return;
    const uint64_t now = NowMs();
    // A piece whose break() already ran here (its own damage, or a repeat of this cue) plays nothing twice.
    if (SeenRecently(p.targetEid, now)) {
        ++g_repeats;
        return;
    }
    for (const Waiting& w : g_waiting)
        if (w.eid == p.targetEid) return;
    if (Play(p.targetEid, false)) return;
    if (g_waiting.size() >= kPendingMax) {
        UE_LOGW("[PIECE-BREAK] client: %zu cues already wait for a copy -- eid=%u is dropped", g_waiting.size(),
                static_cast<unsigned>(p.targetEid));
        ++g_lost;
        return;
    }
    g_waiting.push_back(Waiting{p.targetEid, now + kPendingMs});
}

void OnDisconnect() {
    if (g_sent || g_played || g_repeats || g_ranHere || g_lost)
        UE_LOGI("[PIECE-BREAK] session end -- sent=%u played=%u (after a wait=%u) repeats=%u native-here=%u lost=%u",
                g_sent, g_played, g_late, g_repeats, g_ranHere, g_lost);
    g_sent = g_played = g_repeats = g_ranHere = g_lost = g_noEid = g_late = 0;
    g_recent.clear();
    g_waiting.clear();
    g_applying = 0;
}

}  // namespace coop::piece_break_fx
