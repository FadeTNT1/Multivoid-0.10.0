// coop/props/shrimp_tray_sync.cpp -- see coop/props/shrimp_tray_sync.h.

#include "coop/props/shrimp_tray_sync.h"

#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_food_state.h"
#include "coop/props/prop_hold.h"
#include "coop/props/prop_key_index.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/shrimp_tray.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

namespace coop::props::shrimp_tray_sync {
namespace {

namespace EL = coop::element;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace ST = ue_wrap::shrimp_tray;
namespace SI = coop::net::shrimp_dispense;

UE_FACT_FN(kRowAction, "shrimp_tray_sync", L"prop_food_shrimp_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowHandUse, "shrimp_tray_sync", L"prop_food_shrimp_C", L"playerHandUse_RMB", Declared);
UE_FACT_FN(kRowUpd, "shrimp_tray_sync", L"prop_food_shrimp_C", L"upd", Declared);
constexpr int kTagAction = 0x53485031;   // 'SHP1'
constexpr int kTagHandUse = 0x53485032;
constexpr int kTagUpd = 0x53485033;

constexpr uint8_t kDispenseAction = 4;
constexpr float   kReachUU = 450.0f;
constexpr coop::net::IntentBudget kBudget{4.0f, 4.0f};
constexpr size_t   kMaxPending = 8;
constexpr uint64_t kSayMs = 10000;

std::atomic<coop::net::Session*> g_session{nullptr};
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_regAction = Reg::Pending, g_regHandUse = Reg::Pending, g_regUpd = Reg::Pending;
bool g_settled = false;
bool g_hooked = false;

// The client's own RMB on a tray in its hand: the bit its PRE puts on that body's scope, and the body it put it on.
uintptr_t g_handBit = 0;
void*     g_handFn = nullptr;

// The CLIENT's run of the game's upd from a host state: its POST is not the host's mark.
int g_applying = 0;

R::ParamOffset g_actionParam{L"action"};
R::ParamOffset g_actionPlayer{L"player"};
R::ParamOffset g_handPlayer{L"player"};

uint64_t g_sent = 0, g_ran = 0, g_denied = 0, g_dropped = 0, g_handRuns = 0, g_unkeyed = 0;
bool     g_paramSaid = false;

uint64_t NowMs() { return coop::net::NowMs(); }

bool IsClient(coop::net::Session* s) {
    return s && s->running() && s->connected() && s->role() == coop::net::Role::Client;
}

struct Slot {
    coop::net::IntentQueue<coop::net::ShrimpDispenseIntentPayload> queue;
    bool     waitSaid = false;
    uint64_t nextSayMs = 0;
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

// ---- the food lane's portions -------------------------------------------------------------------------------------
bool ReadPortions(void* food, int16_t& portions) {
    int32_t n = 0;
    if (!ST::ReadCount(food, n)) return false;
    const int32_t cap = ue_wrap::profile::name::ShrimpTrayMaxCount;
    portions = static_cast<int16_t>(n < 0 ? 0 : (n > cap ? cap : n));
    return true;
}

void ApplyPortions(void* food, int16_t portions) {
    int32_t was = 0;
    if (!ST::ReadCount(food, was) || was == portions) return;
    ++g_applying;
    ST::WriteCountAndRedraw(food, portions);
    --g_applying;
    UE_LOGI("[SHRIMP] client: tray key='%ls' now %d portions (was %d)",
            ue_wrap::prop::GetInteractableKeyString(food).c_str(), static_cast<int>(portions), static_cast<int>(was));
}

// ---- client: the presses ------------------------------------------------------------------------------------------
// A tray every peer shares: a keyed world prop outside a hand. One in a hand is that player's own.
bool SharedTray(void* tray, std::wstring& key) {
    if (coop::hand_item::IsHandAxisActor(tray)) return false;
    key = ue_wrap::prop::GetInteractableKeyString(tray);
    return !key.empty() && key != L"None";
}

bool Send(coop::net::Session& s, const std::wstring& key, uint8_t route) {
    if (key.size() > sizeof(coop::net::WireKey::data)) {
        if (++g_unkeyed <= 3)
            UE_LOGW("[SHRIMP] client: tray key='%ls' does not fit the wire -- the press runs here", key.c_str());
        return false;
    }
    coop::net::ShrimpDispenseIntentPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.route = route;
    if (!s.SendReliable(coop::net::ReliableKind::ShrimpDispenseIntent, &p, sizeof(p))) {
        UE_LOGW("[SHRIMP] client: the press on tray key='%ls' was not sent -- it runs here", key.c_str());
        return false;
    }
    ++g_sent;
    UE_LOGI("[SHRIMP] client: sent %s on tray key='%ls' (#%llu)", route == SI::kHandUse ? "the hand use" : "action 4",
            key.c_str(), static_cast<unsigned long long>(g_sent));
    return true;
}

void* ParamObject(R::ParamOffset& off, const sg::Call& c) {
    const int32_t o = off.Of(c.function);
    if (o < 0 || !c.locals) return nullptr;
    return *reinterpret_cast<void* const*>(c.locals + o);
}

sg::Verdict OnActionPre(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!IsClient(s) || !c.object) return sg::Verdict::Run;
    const int32_t off = g_actionParam.Of(c.function);
    if (off < 0 || !c.locals) {
        if (!g_paramSaid) {
            g_paramSaid = true;
            UE_LOGW("[SHRIMP] the tray press's action parameter did not resolve -- a press runs here alone (said once)");
        }
        return sg::Verdict::Run;
    }
    if (c.locals[off] != kDispenseAction) return sg::Verdict::Run;
    void* player = ParamObject(g_actionPlayer, c);
    if (player && !coop::players::Registry::Get().IsLocal(player)) return sg::Verdict::Run;
    std::wstring key;
    if (!SharedTray(c.object, key)) return sg::Verdict::Run;
    return Send(*s, key, SI::kAction) ? sg::Verdict::Cancel : sg::Verdict::Run;
}

sg::Verdict OnHandUsePre(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!IsClient(s) || !c.object) return sg::Verdict::Run;
    void* player = ParamObject(g_handPlayer, c);
    if (player && !coop::players::Registry::Get().IsLocal(player)) return sg::Verdict::Run;
    std::wstring key;
    if (SharedTray(c.object, key)) return Send(*s, key, SI::kHandUse) ? sg::Verdict::Cancel : sg::Verdict::Run;
    // The player's own tray: its portion is its own, and what it spawns is this player's birth.
    if (g_handBit != 0) {
        sg::MarkScope(c, g_handBit);
        g_handFn = c.function;
    }
    ++g_handRuns;
    return sg::Verdict::Run;
}

// HOST: the tray redrew, so its count moved (a press, a loadData): its state goes out with the new portions.
void OnUpdPost(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Host || g_applying > 0 || !c.object) return;
    coop::props::prop_food_state::MarkDirty(c.object);
}

// ---- host: the judge ----------------------------------------------------------------------------------------------
void Deny(uint8_t slot, const std::wstring& key, const char* why) {
    ++g_denied;
    Slot& sl = g_slots[slot];
    const uint64_t now = NowMs();
    if (now < sl.nextSayMs) return;
    sl.nextSayMs = now + kSayMs;
    UE_LOGI("[SHRIMP] host: DENY slot %u's press on tray key='%ls' -- %s", static_cast<unsigned>(slot), key.c_str(), why);
}

// False keeps the press at the head of its sender's queue: the host has no body for the sender yet.
bool Execute(coop::net::Session& s, const coop::net::ShrimpDispenseIntentPayload& p, uint8_t slot) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* tray = coop::prop_key_index::Find(key);
    if (!tray || !ST::IsTray(tray)) {
        Deny(slot, key, "no tray holds the key");
        return true;
    }
    if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) {
        Deny(slot, key, "the sender's player is not alive");
        return true;
    }
    const EL::IntentSubject subject = EL::IntentTarget::ForClientIntent(s, slot, kReachUU).Authorize(tray);
    if (subject.outcome == EL::IntentOutcome::NoBody) {
        if (!g_slots[slot].waitSaid) {
            g_slots[slot].waitSaid = true;
            UE_LOGI("[SHRIMP] host: slot %u's presses wait: the host has no body for it yet", static_cast<unsigned>(slot));
        }
        return false;
    }
    if (!subject) {
        Deny(slot, key, EL::OutcomeName(subject.outcome));
        return true;
    }
    if (const coop::prop_hold::Holder h = coop::prop_hold::WhoHolds(tray, slot); h != coop::prop_hold::Holder::None) {
        Deny(slot, key, coop::prop_hold::HolderWhy(h));
        return true;
    }
    int32_t count = 0;
    if (!ST::ReadCount(tray, count) || count <= 0) {
        Deny(slot, key, "the tray is empty or its count is unreadable");
        return true;
    }
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    void* body = (rp && rp->valid()) ? rp->GetActor() : nullptr;
    void* dropCls = ST::DropClass(tray);
    const bool ran = p.route == SI::kHandUse ? ST::CallHandUse(tray, body)
                                             : ST::CallDispenseAction(tray, body, kDispenseAction);
    if (!ran) {
        Deny(slot, key, "the host's copy did not take the call");
        return true;
    }
    ++g_ran;
    UE_LOGI("[SHRIMP] host: ran slot %u's %s on tray key='%ls' -- %d portions before, drop class '%ls' (#%llu)",
            static_cast<unsigned>(slot), p.route == SI::kHandUse ? "hand use" : "action 4", key.c_str(),
            static_cast<int>(count), dropCls ? R::ToString(R::NameOf(dropCls)).c_str() : L"(none)",
            static_cast<unsigned long long>(g_ran));
    return true;
}

void RegisterWatches() {
    if (g_regAction == Reg::Pending)
        g_regAction = sg::WatchName(kRowAction, kTagAction, &OnActionPre, nullptr) ? Reg::Registered : Reg::Refused;
    if (g_regHandUse == Reg::Pending)
        g_regHandUse = sg::WatchName(kRowHandUse, kTagHandUse, &OnHandUsePre, nullptr) ? Reg::Registered : Reg::Refused;
    if (g_regUpd == Reg::Pending)
        g_regUpd = sg::WatchName(kRowUpd, kTagUpd, nullptr, &OnUpdPost) ? Reg::Registered : Reg::Refused;
}

void Settle() {
    if (g_settled) return;
    // Tick can run before Install has asked for the watches; an empty pending table then says nothing.
    if (g_regAction == Reg::Pending || g_regHandUse == Reg::Pending || g_regUpd == Reg::Pending) return;
    const bool live = g_regAction == Reg::Registered && g_regHandUse == Reg::Registered &&
                      g_regUpd == Reg::Registered && sg::NameWatchLive(kRowAction, kTagAction) &&
                      sg::NameWatchLive(kRowHandUse, kTagHandUse) && sg::NameWatchLive(kRowUpd, kTagUpd);
    if (live) {
        g_settled = true;
        UE_LOGI("[SHRIMP] the tray's press, hand use and redraw watches are live");
    } else if (sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[SHRIMP] a tray watch is dead (refused, or its class declares no such body) -- a tray press runs only "
                "where it is made");
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!session) return;
    if (g_handBit == 0) g_handBit = sg::ScopeBitFor(&g_handBit);
    RegisterWatches();
    if (!g_hooked) {
        g_hooked = true;
        coop::props::prop_food_state::SetPortions(&ReadPortions, &ApplyPortions);
    }
}

void Tick(coop::net::Session& session) {
    if (!g_settled) {
        sg::ResolvePendingNames();
        Settle();
    }
    if (!session.running() || session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        Slot& sl = g_slots[slot];
        sl.queue.Serve(slot, kBudget, NowMs, [&](const coop::net::ShrimpDispenseIntentPayload& p) {
            if (!Execute(session, p, slot)) return coop::net::Served::Wait;
            sl.waitSaid = false;
            return coop::net::Served::Done;
        });
    }
}

void OnIntent(coop::net::Session& session, const coop::net::ShrimpDispenseIntentPayload& p, uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host || senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    Slot& sl = g_slots[senderSlot];
    auto copy = p;
    if (!sl.queue.Push(std::move(copy), kMaxPending)) Deny(senderSlot, coop::net::StringFromWireKey(p.key), "queue full");
}

void CountDropped() { ++g_dropped; }

bool HandDispenseRunning() {
    return g_handFn && g_handBit != 0 && (sg::ScopeBits(g_handFn) & g_handBit) != 0;
}

void OnDisconnect() {
    if (g_sent || g_ran || g_denied || g_dropped || g_handRuns)
        UE_LOGI("[SHRIMP] session end -- presses sent=%llu run=%llu denied=%llu dropped=%llu own-hand runs=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_ran),
                static_cast<unsigned long long>(g_denied), static_cast<unsigned long long>(g_dropped),
                static_cast<unsigned long long>(g_handRuns));
    g_sent = g_ran = g_denied = g_dropped = g_handRuns = g_unkeyed = 0;
    g_paramSaid = false;
    g_applying = 0;
}

}  // namespace coop::props::shrimp_tray_sync
