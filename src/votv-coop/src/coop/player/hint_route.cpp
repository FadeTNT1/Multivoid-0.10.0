// coop/player/hint_route.cpp -- see coop/player/hint_route.h.

#include "coop/player/hint_route.h"

#include "coop/creatures/served_player.h"
#include "coop/net/acting_slot.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/text/utf8_codec.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/drone.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/votv_lib.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace coop::hint_route {
namespace {

namespace sg = ue_wrap::script_gate;
namespace VL = ue_wrap::votv_lib;
namespace AS = coop::acting_slot;

using coop::players::kPeerIdHost;

// The one function every hint runs through (mainGamemode_C's "Add Hint from Gamemode" forwards to it).
UE_FACT_FN(kRowAddHint, "hint_route", L"lib_C", L"addHint", Declared);
constexpr int kTagAddHint = 0x48525431;  // 'HRT' 1

// One hint text to one slot is sent once in this window: a burst of the same press is one hint on its screen.
constexpr uint64_t kRepeatWindowMs = 2000;

// The credited actors: few at once (the drone, a robot a lane names), so a flat table.
constexpr int kMaxCredits = 8;
struct CreditRec {
    ue_wrap::CachedObjRef actor;
    uint8_t slot = kPeerIdHost;
};
CreditRec g_credits[kMaxCredits];

// The last hint sent to each slot, for the repeat window.
struct Last {
    std::wstring text;
    uint8_t      type = 0;
    uint64_t     atMs = 0;
};
Last g_last[coop::net::kMaxPeers];
// CLIENT: the last hint this machine's own game raised. A body the client also ran itself (a lane that lets both
// run) shows its hint here already, so the host's routed copy of it is not shown a second time.
Last g_localLast;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false;
bool g_saidLive = false;
bool g_saidNoWorld = false;
unsigned long long g_routed = 0, g_coalesced = 0, g_shown = 0, g_failed = 0;

coop::net::Session* HostSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->role() == coop::net::Role::Host) ? s : nullptr;
}

bool IsClientSlot(uint8_t slot) { return slot >= 1 && slot < coop::net::kMaxPeers; }

uint8_t CreditOf(void* obj) {
    if (!obj) return AS::kNone;
    for (const CreditRec& c : g_credits)
        if (c.actor.Is(obj) && c.actor.Alive()) return c.slot;
    return AS::kNone;
}

// The client an actor's own job serves: the robot a client commands, or the drone a client sent, by the sell point
// that names it. kNone for anything else.
uint8_t ServedBy(void* obj) {
    if (!obj) return AS::kNone;
    const uint8_t robot = coop::served_player::ServedSlot(obj);
    if (IsClientSlot(robot)) return robot;
    const uint8_t credited = CreditOf(obj);
    if (credited != AS::kNone) return credited;
    if (void* drone = ue_wrap::drone::OfSellLocationIfAny(obj)) return CreditOf(drone);
    return AS::kNone;
}

// Whose hint this call is: the open acting scope's slot, else (no scope) the client its raiser is working for,
// unless the chain began at the host's own player. kNone or 0 keeps it on the host.
uint8_t OwnerOf(const sg::Call& c, const VL::HintCall& h, const char*& why) {
    const uint8_t acting = AS::Current();
    if (acting != AS::kNone) {
        why = "acting";
        return acting;
    }
    if (VL::BeganAtPlayer(c)) return AS::kNone;  // the host's own hand
    why = "served";
    uint8_t slot = ServedBy(h.worldContext);
    if (slot == AS::kNone && c.callerObject != h.worldContext) slot = ServedBy(c.callerObject);
    return slot;
}

bool OnClient() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Client;
}

sg::Verdict OnAddHintPre(const sg::Call& c) {
    coop::net::Session* s = HostSession();
    if (!s) {
        if (OnClient()) {
            VL::HintCall own;
            if (VL::ReadAddHintCall(c, own) && !own.debug) {
                g_localLast.text = std::move(own.text);
                g_localLast.type = own.type;
                g_localLast.atMs = coop::net::NowMs();
            }
        }
        return sg::Verdict::Run;
    }
    VL::HintCall h;
    if (!VL::ReadAddHintCall(c, h)) return sg::Verdict::Run;
    if (h.debug) return sg::Verdict::Run;  // editor-only: the game shows it nowhere outside the editor
    const char* why = "";
    const uint8_t slot = OwnerOf(c, h, why);
    if (!IsClientSlot(slot)) return sg::Verdict::Run;  // the host's own, or nobody's but the host's

    const uint64_t now = coop::net::NowMs();
    Last& last = g_last[slot];
    if (last.type == h.type && last.text == h.text && now - last.atMs < kRepeatWindowMs) {
        last.atMs = now;
        ++g_coalesced;
        UE_LOGI("hint_route: hint to slot %u repeated within %llu ms, sent once: '%ls'", static_cast<unsigned>(slot),
                static_cast<unsigned long long>(kRepeatWindowMs), h.text.c_str());
        return sg::Verdict::Cancel;
    }

    coop::net::HintRoutePayload p{};
    const std::string utf8 = coop::text::CapUtf8Bytes(coop::text::ToUtf8(h.text), sizeof(p.text));
    if (utf8.empty()) return sg::Verdict::Run;
    p.type = h.type;
    p.len = static_cast<uint8_t>(utf8.size());
    std::memcpy(p.text, utf8.data(), utf8.size());
    if (!s->SendReliableToSlot(slot, coop::net::ReliableKind::HintRoute, &p, sizeof(p))) {
        ++g_failed;
        UE_LOGW("hint_route: hint for slot %u could not be sent -- shown on the host: '%ls'",
                static_cast<unsigned>(slot), h.text.c_str());
        return sg::Verdict::Run;
    }
    last.text = h.text;
    last.type = h.type;
    last.atMs = now;
    ++g_routed;
    UE_LOGI("hint routed to slot %u: '%ls' (type %u, %s)", static_cast<unsigned>(slot), h.text.c_str(),
            static_cast<unsigned>(h.type), why);
    return sg::Verdict::Cancel;
}

void Register() {
    if (!g_watched) g_watched = sg::WatchName(kRowAddHint, kTagAddHint, &OnAddHintPre, nullptr);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    Register();
}

void Tick() {
    if (g_saidLive) return;
    Register();
    if (!g_watched) return;
    sg::ResolvePendingNames();
    if (sg::NameWatchLive(kRowAddHint, kTagAddHint)) {
        g_saidLive = true;
        UE_LOGI("hint_route: lib_C::addHint is watched -- the host's hints for a client go to that client");
    } else if (sg::NameWatchSettled(kRowAddHint, kTagAddHint)) {
        g_saidLive = true;
        UE_LOGE("hint_route: the lib_C::addHint watch died at the name resolve -- hints stay where the game raises "
                "them");
    }
}

void Credit(void* actor, uint8_t slot) {
    if (!actor) return;
    CreditRec* open = nullptr;
    for (CreditRec& c : g_credits) {
        if (c.actor.Is(actor)) {
            if (IsClientSlot(slot)) {
                c.slot = slot;
            } else {
                c.actor.Reset();
                c.slot = kPeerIdHost;
            }
            return;
        }
        if (!open && !c.actor.Alive()) open = &c;
    }
    if (!IsClientSlot(slot)) return;
    if (!open) {
        UE_LOGW("hint_route: no room to credit %p to slot %u -- its later hints stay on the host", actor,
                static_cast<unsigned>(slot));
        return;
    }
    open->actor.Set(actor);
    open->slot = slot;
}

void OnRouted(const coop::net::HintRoutePayload& payload) {
    const size_t len = payload.len <= sizeof(payload.text) ? payload.len : sizeof(payload.text);
    const std::wstring text = coop::text::FromUtf8Lossy(payload.text, len);
    if (text.empty()) return;
    if (g_localLast.type == payload.type && g_localLast.text == text &&
        coop::net::NowMs() - g_localLast.atMs < kRepeatWindowMs) {
        ++g_coalesced;
        UE_LOGI("hint_route: the host's hint is already on screen here (this game raised it too): '%ls'", text.c_str());
        return;
    }
    void* const local = coop::players::Registry::Get().Local();
    if (!local) {
        if (!g_saidNoWorld) {
            g_saidNoWorld = true;
            UE_LOGW("hint_route: a hint from the host arrived with no local player -- dropped: '%ls'", text.c_str());
        }
        return;
    }
    if (!VL::AddHint(text, payload.type, local)) {
        ++g_failed;
        UE_LOGW("hint_route: the host's hint did not show here (lib_C::addHint unresolved): '%ls'", text.c_str());
        return;
    }
    ++g_shown;
    UE_LOGI("hint_route: showed the host's hint for this player: '%ls' (type %u)", text.c_str(),
            static_cast<unsigned>(payload.type));
}

void OnPeerLeft(uint8_t slot) {
    if (!IsClientSlot(slot)) return;
    for (CreditRec& c : g_credits)
        if (c.slot == slot) {
            c.actor.Reset();
            c.slot = kPeerIdHost;
        }
    g_last[slot] = Last{};
}

void OnDisconnect() {
    if (g_routed || g_coalesced || g_shown || g_failed)
        UE_LOGI("hint_route: session end -- routed=%llu coalesced=%llu shown=%llu failed=%llu", g_routed, g_coalesced,
                g_shown, g_failed);
    for (CreditRec& c : g_credits) {
        c.actor.Reset();
        c.slot = kPeerIdHost;
    }
    for (Last& l : g_last) l = Last{};
    g_localLast = Last{};
    g_routed = g_coalesced = g_shown = g_failed = 0;
    g_saidNoWorld = false;
}

}  // namespace coop::hint_route
