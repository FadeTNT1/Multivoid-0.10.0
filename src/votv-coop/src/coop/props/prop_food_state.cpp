// coop/props/prop_food_state.cpp -- see coop/props/prop_food_state.h.

#include "coop/props/prop_food_state.h"

#include "coop/element/element.h"
#include "coop/element/intent_authority.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_save_data.h"

#include "ue_wrap/actors/food.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::props::prop_food_state {
namespace {

namespace EL = coop::element;
namespace FD = ue_wrap::food;
namespace FS = coop::net::food_state_flags;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

// ---- the watches ----------------------------------------------------------------------------------------------
// A Declared row watches the body its class owns, which every class below that inherits it shares: prop_food_C's
// tick is the tick of every food but the three that override it. prop_cookingFood_C's override calls the base tick
// first and then cooks; the blue brain cheese's and the coffee cup's replace it.
UE_FACT_FN(kRowTick, "prop_food_state", L"prop_food_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowCookTick, "prop_food_state", L"prop_cookingFood_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowCheeseTick, "prop_food_state", L"prop_food_bluebraincheese_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowCupTick, "prop_food_state", L"prop_food_coffeecup_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowAddTemp, "prop_food_state", L"prop_food_C", L"addTemperature", Declared);
UE_FACT_FN(kRowAccTemp, "prop_food_state", L"prop_food_C", L"accumulateTemperature", Declared);
UE_FACT_FN(kRowMicrowave, "prop_food_state", L"prop_food_C", L"microwave", Declared);
UE_FACT_FN(kRowBitten, "prop_food_state", L"prop_food_C", L"bitten", Declared);
// cookItem: the cooking food's own and the overrides of its subclasses (the shrimp, the cookie tray), reached from the
// cooking tick, the cookinator, the fish's decimate and the food-cook tool. Rare, so every class's body is watched
// and the food is asked in the callback.
UE_FACT_FN(kRowCookItem, "prop_food_state", nullptr, L"cookItem", AnyClass);

enum class Role : uint8_t { Tick, Heat, CookItem, Bitten };
struct Watch {
    const ue_wrap::facts::Function* row;
    int                             tag;
    Role                            role;
    const char*                     what;
};
const Watch kWatches[] = {
    {&kRowTick, 0x46445431 /*'FDT1'*/, Role::Tick, "prop_food_C.ReceiveTick"},
    {&kRowCookTick, 0x46445432, Role::Tick, "prop_cookingFood_C.ReceiveTick"},
    {&kRowCheeseTick, 0x46445433, Role::Tick, "prop_food_bluebraincheese_C.ReceiveTick"},
    {&kRowCupTick, 0x46445434, Role::Tick, "prop_food_coffeecup_C.ReceiveTick"},
    {&kRowAddTemp, 0x46445435, Role::Heat, "prop_food_C.addTemperature"},
    {&kRowAccTemp, 0x46445436, Role::Heat, "prop_food_C.accumulateTemperature"},
    {&kRowMicrowave, 0x46445437, Role::Heat, "prop_food_C.microwave"},
    {&kRowCookItem, 0x46445438, Role::CookItem, "cookItem"},
    {&kRowBitten, 0x46445439, Role::Bitten, "prop_food_C.bitten"},
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));

enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kWatchCount] = {};
bool g_settled = false;

std::atomic<coop::net::Session*> g_session{nullptr};

coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

bool IsClient(coop::net::Session* s) {
    return s && s->running() && s->connected() && s->role() == coop::net::Role::Client;
}
bool IsHost(coop::net::Session* s) { return s && s->running() && s->role() == coop::net::Role::Host; }

const Watch* WatchOf(int tag) {
    for (const Watch& w : kWatches)
        if (w.tag == tag) return &w;
    return nullptr;
}

// ---- the steps --------------------------------------------------------------------------------------------------
constexpr float    kTempStep = 0.5f;       // kelvin: the room's own jitter is a third of a kelvin
constexpr float    kFreshStep = 0.02f;
constexpr float    kCookStep = 0.5f;
constexpr uint64_t kPeriodMs = 1000;
constexpr size_t   kPumpPerTick = 48;      // foods read a tick, round the table
constexpr size_t   kSeedPerTick = 24;      // a joiner's states sent a tick
constexpr size_t   kMaxFoods = 4096;
constexpr float    kReachUU = 450.0f;
constexpr uint64_t kSayMs = 10000;
constexpr int16_t  kMaxPortions = 64;  // a tray's count on the wire: far over the nine it holds

uint64_t NowMs() { return coop::net::NowMs(); }

// Is a food one every peer shares? A tracked world prop, or a keyed one outside a hand. A hand's food is its holder's
// own (coop/player/hand_item), and a fresh local birth not yet tracked or keyed is this peer's until it is.
bool IsShared(void* food) {
    if (EL::Registry::Get().EidForActor(food) != EL::kInvalidId) return true;
    if (coop::hand_item::IsHandAxisActor(food)) return false;
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(food);
    return !key.empty() && key != L"None";
}

// The client's answer per food, kept two seconds: the tick asks every frame.
struct SharedMemo {
    int32_t  idx = -1;
    bool     shared = false;
    uint64_t untilMs = 0;
};
std::unordered_map<void*, SharedMemo> g_sharedMemo;

bool IsSharedMemo(void* food) {
    const uint64_t now = NowMs();
    const int32_t idx = R::InternalIndexOf(food);
    const auto it = g_sharedMemo.find(food);
    if (it != g_sharedMemo.end() && it->second.idx == idx && now < it->second.untilMs) return it->second.shared;
    if (g_sharedMemo.size() > kMaxFoods * 2) g_sharedMemo.clear();
    SharedMemo& slot = g_sharedMemo[food];
    slot.idx = idx;
    slot.shared = IsShared(food);
    slot.untilMs = now + 2000;
    return slot.shared;
}

// ---- host: the table --------------------------------------------------------------------------------------------
struct HostRec {
    ue_wrap::CachedObjRef actor;
    FD::State             last;
    bool                  sent = false;
    bool                  dirty = false;
    uint64_t              sentMs = 0;
};
std::vector<HostRec> g_foods;
std::unordered_map<void*, size_t> g_foodIndex;
size_t g_cursor = 0;
bool   g_fullSaid = false;

// A joiner's seed: the foods noted at its ready edge, sent a few a tick.
std::vector<ue_wrap::CachedObjRef> g_seed[coop::net::kMaxPeers];

uint64_t g_published = 0, g_seeded = 0, g_applied = 0, g_parked = 0, g_cancelled = 0, g_bitesSent = 0,
         g_bitesTaken = 0, g_bitesRefused = 0, g_dropSize = 0, g_dropMalformed = 0, g_notFood = 0, g_unread = 0;
bool g_cancelSaid = false, g_publishSaid = false, g_applySaid = false, g_noKeySaid = false;
int  g_editSaid = 0;

void Enroll(void* food) {
    const auto it = g_foodIndex.find(food);
    if (it != g_foodIndex.end()) {
        HostRec& r = g_foods[it->second];
        if (r.actor.Is(food)) return;
        r = HostRec{};   // a new food at a dead one's address
        r.actor.Set(food);
        return;
    }
    if (g_foods.size() >= kMaxFoods) {
        if (!g_fullSaid) {
            g_fullSaid = true;
            UE_LOGW("[FOOD] host: %zu foods are noted already -- a further food's state is not published (said once)",
                    kMaxFoods);
        }
        return;
    }
    HostRec r;
    r.actor.Set(food);
    g_foodIndex[food] = g_foods.size();
    g_foods.push_back(std::move(r));
}

void Forget(size_t i) {
    void* raw = g_foods[i].actor.Raw();
    g_foodIndex.erase(raw);
    if (i + 1 != g_foods.size()) {
        g_foods[i] = std::move(g_foods.back());
        g_foodIndex[g_foods[i].actor.Raw()] = i;
    }
    g_foods.pop_back();
}

bool Fill(void* food, const FD::State& st, coop::net::FoodStatePayload& p) {
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(food);
    if (key.empty() || key == L"None" || key.size() > sizeof(p.key.data)) return false;
    p = coop::net::FoodStatePayload{};
    coop::net::WireKeyFromString(key, p.key);
    p.temperature = st.temperature;
    p.freshness = st.freshness;
    p.cookProgress = st.hasCook ? st.cookProgress : 0.f;
    p.uses = static_cast<int16_t>(st.uses < -1 ? -1 : (st.uses > 30000 ? 30000 : st.uses));
    p.freshnessLeft = static_cast<int16_t>(st.freshnessLeft < -1 ? -1 : (st.freshnessLeft > 30000 ? 30000 : st.freshnessLeft));
    p.portions = -1;
    p.flags = static_cast<uint8_t>((st.ignoreRotting ? FS::kIgnoreRotting : 0) | (st.spoiled ? FS::kSpoiled : 0) |
                                   (st.frozen ? FS::kFrozen : 0) | (st.hasCook ? FS::kCooking : 0));
    return true;
}

bool Edge(const FD::State& a, const FD::State& b) {
    return a.uses != b.uses || a.freshnessLeft != b.freshnessLeft || a.spoiled != b.spoiled || a.frozen != b.frozen ||
           a.ignoreRotting != b.ignoreRotting || a.hasCook != b.hasCook;
}

bool Stepped(const FD::State& a, const FD::State& b) {
    return std::fabs(a.temperature - b.temperature) >= kTempStep || std::fabs(a.freshness - b.freshness) >= kFreshStep ||
           (a.hasCook && std::fabs(a.cookProgress - b.cookProgress) >= kCookStep);
}

void Publish(coop::net::Session& s, HostRec& r, void* food, const FD::State& st, uint64_t now) {
    coop::net::FoodStatePayload p{};
    if (!Fill(food, st, p)) {
        if (!g_noKeySaid) {
            g_noKeySaid = true;
            UE_LOGW("[FOOD] host: a food has no save key a peer could find it by -- its state is not published (said "
                    "once)");
        }
        r.sent = true;   // nothing to send it by; asked again only on its next edge or step
        r.last = st;
        r.sentMs = now;
        return;
    }
    if (!s.SendReliable(coop::net::ReliableKind::FoodState, &p, sizeof(p))) return;   // no peer: the seed carries it
    ++g_published;
    if (!g_publishSaid) {
        g_publishSaid = true;
        UE_LOGI("[FOOD] host: the first food state went out -- key='%ls' %.1f K fresh=%.3f uses=%d",
                coop::net::StringFromWireKey(p.key).c_str(), static_cast<double>(p.temperature),
                static_cast<double>(p.freshness), static_cast<int>(p.uses));
    }
    r.sent = true;
    r.dirty = false;
    r.last = st;
    r.sentMs = now;
}

void PumpHost(coop::net::Session& s) {
    if (g_foods.empty()) return;
    const bool anyPeer = s.connected() && s.AnyWorldReadyPeer();
    const uint64_t now = NowMs();
    size_t budget = kPumpPerTick < g_foods.size() ? kPumpPerTick : g_foods.size();
    while (budget-- > 0 && !g_foods.empty()) {
        if (g_cursor >= g_foods.size()) g_cursor = 0;
        HostRec& r = g_foods[g_cursor];
        void* food = r.actor.Get();
        FD::State st;
        if (!food || !FD::ReadState(food, st)) {
            if (food) ++g_unread;
            Forget(g_cursor);
            continue;
        }
        ++g_cursor;
        if (!anyPeer || coop::hand_item::IsHandAxisActor(food)) continue;
        const bool due = !r.sent || r.dirty || Edge(st, r.last) || (Stepped(st, r.last) && now - r.sentMs >= kPeriodMs);
        if (due) Publish(s, r, food, st, now);
    }
}

void PumpSeeds(coop::net::Session& s) {
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        std::vector<ue_wrap::CachedObjRef>& q = g_seed[slot];
        if (q.empty()) continue;
        if (!s.IsSlotWorldReady(slot)) {
            q.clear();
            continue;
        }
        size_t n = 0;
        while (!q.empty() && n < kSeedPerTick) {
            void* food = q.back().Get();
            q.pop_back();
            FD::State st;
            coop::net::FoodStatePayload p{};
            if (!food || coop::hand_item::IsHandAxisActor(food) || !FD::ReadState(food, st) || !Fill(food, st, p))
                continue;
            if (s.SendReliableToSlot(slot, coop::net::ReliableKind::FoodState, &p, sizeof(p))) {
                ++g_seeded;
                ++n;
            }
        }
    }
}

// ---- client: the apply ------------------------------------------------------------------------------------------
FD::State StateOf(const coop::net::FoodStatePayload& p) {
    FD::State st;
    st.temperature = p.temperature;
    st.freshness = p.freshness;
    st.cookProgress = p.cookProgress;
    st.uses = p.uses;
    st.freshnessLeft = p.freshnessLeft;
    st.ignoreRotting = (p.flags & FS::kIgnoreRotting) != 0;
    st.spoiled = (p.flags & FS::kSpoiled) != 0;
    st.frozen = (p.flags & FS::kFrozen) != 0;
    st.hasCook = (p.flags & FS::kCooking) != 0;
    return st;
}

// False: no prop holds the key here yet (parked by the caller).
bool Apply(const coop::net::FoodStatePayload& p) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* food = coop::prop_key_index::Find(key);
    if (!food) return false;
    FD::State was;
    if (!FD::ReadState(food, was)) {
        ++g_notFood;
        return true;
    }
    const FD::State now = StateOf(p);
    if (!FD::WriteState(food, now)) {
        ++g_notFood;
        return true;
    }
    FD::RefreshDisplay(food, now, was);
    ++g_applied;
    if (!g_applySaid) {
        g_applySaid = true;
        UE_LOGI("[FOOD] client: the first food state landed -- key='%ls' %.1f K fresh=%.3f uses=%d", key.c_str(),
                static_cast<double>(p.temperature), static_cast<double>(p.freshness), static_cast<int>(p.uses));
    }
    return true;
}

// States for a key no prop holds here yet (a birth whose spawn is still on its way), latest wins, tried again when the
// key index moves and dropped after a while.
struct Parked {
    coop::net::FoodStatePayload p;
    uint64_t                    untilMs;
};
std::unordered_map<std::wstring, Parked> g_parkedStates;
constexpr size_t   kMaxParked = 256;
constexpr uint64_t kParkMs = 15000;
uint64_t g_parkVersion = 0;

void Park(const coop::net::FoodStatePayload& p) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (g_parkedStates.size() >= kMaxParked && g_parkedStates.find(key) == g_parkedStates.end()) return;
    g_parkedStates[key] = Parked{p, NowMs() + kParkMs};
    ++g_parked;
}

void RetryParked() {
    if (g_parkedStates.empty()) return;
    const uint64_t v = coop::prop_key_index::Version();
    const uint64_t now = NowMs();
    if (v == g_parkVersion) {
        for (auto it = g_parkedStates.begin(); it != g_parkedStates.end();)
            it = now >= it->second.untilMs ? g_parkedStates.erase(it) : std::next(it);
        return;
    }
    g_parkVersion = v;
    for (auto it = g_parkedStates.begin(); it != g_parkedStates.end();) {
        if (Apply(it->second.p) || now >= it->second.untilMs) it = g_parkedStates.erase(it);
        else ++it;
    }
}

// ---- the callbacks ----------------------------------------------------------------------------------------------
sg::Verdict OnPre(const sg::Call& c) {
    coop::net::Session* s = LoadSession();
    if (!IsClient(s) || !c.object) return sg::Verdict::Run;
    const Watch* w = WatchOf(c.tag);
    if (!w || w->role == Role::Bitten) return sg::Verdict::Run;
    if (w->role == Role::CookItem && !FD::IsFood(c.object)) return sg::Verdict::Run;
    if (!IsSharedMemo(c.object)) return sg::Verdict::Run;
    ++g_cancelled;
    if (!g_cancelSaid) {
        g_cancelSaid = true;
        UE_LOGI("[FOOD] client: a shared food's %s is the host's -- cancelled here (its state arrives as FoodState; "
                "said once)", w->what);
    }
    return sg::Verdict::Cancel;
}

uint64_t g_biteWindowMs = 0;
int      g_biteWindowN = 0;

void SendBite(coop::net::Session& s, void* food) {
    FD::State st;
    if (!FD::ReadState(food, st) || st.uses <= 0 || st.uses > 30000) return;   // the finishing bite is prop_act's
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(food);
    if (key.empty() || key == L"None" || key.size() > sizeof(coop::net::WireKey::data)) return;
    const uint64_t now = NowMs();
    if (now >= g_biteWindowMs) {
        g_biteWindowMs = now + 1000;
        g_biteWindowN = 0;
    }
    if (++g_biteWindowN > 4) return;
    coop::net::FoodBiteIntentPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.usesAfter = static_cast<int16_t>(st.uses);
    if (!s.SendReliable(coop::net::ReliableKind::FoodBiteIntent, &p, sizeof(p))) return;
    ++g_bitesSent;
    UE_LOGI("[FOOD] client: sent a bite of key='%ls', %d left", key.c_str(), static_cast<int>(st.uses));
}

void OnPost(const sg::Call& c) {
    coop::net::Session* s = LoadSession();
    if (!s || !s->running() || !c.object) return;
    const Watch* w = WatchOf(c.tag);
    if (!w) return;
    if (s->role() == coop::net::Role::Host) {
        if (w->role == Role::Tick) Enroll(c.object);
        else if (w->role == Role::Bitten) MarkDirty(c.object);
        return;
    }
    if (w->role == Role::Bitten && IsClient(s) && IsSharedMemo(c.object)) SendBite(*s, c.object);
}

void RegisterWatches() {
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        const Watch& w = kWatches[i];
        const bool pre = w.role != Role::Bitten;
        const bool post = w.role == Role::Tick || w.role == Role::Bitten;
        if (sg::WatchName(*w.row, w.tag, pre ? &OnPre : nullptr, post ? &OnPost : nullptr)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("[FOOD] the script-body gate refused the watch on %s -- that part of a food runs on every peer", w.what);
    }
}

void Settle() {
    if (g_settled) return;
    int live = 0, pending = 0;
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] == Reg::Pending) ++pending;
        else if (g_reg[i] == Reg::Registered && sg::NameWatchLive(*kWatches[i].row, kWatches[i].tag)) ++live;
    }
    if (live == kWatchCount) {
        g_settled = true;
        UE_LOGI("[FOOD] the food watches are live: the ticks, the heating, cookItem and the bite");
        return;
    }
    if (pending == 0 && sg::PendingNameCount() == 0) {
        g_settled = true;
        for (int i = 0; i < kWatchCount; ++i)
            if (g_reg[i] != Reg::Registered || !sg::NameWatchLive(*kWatches[i].row, kWatches[i].tag))
                UE_LOGE("[FOOD] the %s watch is dead -- that part of a food runs on every peer", kWatches[i].what);
    }
}

void SayDenied(const char* why, const std::wstring& key, uint8_t slot) {
    ++g_bitesRefused;
    static uint64_t s_nextMs = 0;
    const uint64_t now = NowMs();
    if (now < s_nextMs) return;
    s_nextMs = now + kSayMs;
    UE_LOGI("[FOOD] host: DENY slot %u's bite of key='%ls' -- %s", static_cast<unsigned>(slot), key.c_str(), why);
}

}  // namespace

void Install(coop::net::Session* session) {
    // One name covers the whole family on the record lane: the claim resolves against the class and its descendants,
    // and every class that carries this state is one of them, including the ones whose names say nothing about food
    // (prop_bananaHusk_C, prop_arirEgg_C). prop_food_mre_C is a prop_C and carries none of it.
    coop::prop_save_data::DeclareClassOwnedElsewhere(ue_wrap::profile::name::FoodClass);
    g_session.store(session, std::memory_order_release);
    if (session) RegisterWatches();
}

void Tick(coop::net::Session& session) {
    if (!g_settled) {
        sg::ResolvePendingNames();
        Settle();
    }
    if (!session.running()) return;
    if (session.role() == coop::net::Role::Host) {
        PumpHost(session);
        PumpSeeds(session);
    } else {
        RetryParked();
    }
}

void OnPeerWorldReady(int slot) {
    coop::net::Session* s = LoadSession();
    if (!IsHost(s) || slot < 1 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    std::vector<ue_wrap::CachedObjRef>& q = g_seed[slot];
    q.clear();
    for (const HostRec& r : g_foods) {
        if (!r.actor.Alive()) continue;
        q.push_back(r.actor);
    }
    if (!q.empty())
        UE_LOGI("[FOOD] host: slot %d's world is up -- %zu food states to send, %zu a tick", slot, q.size(), kSeedPerTick);
}

void OnState(const coop::net::FoodStatePayload& p) {
    if (!IsClient(LoadSession())) return;
    if (!Apply(p)) Park(p);
}

void OnBite(coop::net::Session& session, const coop::net::FoodBiteIntentPayload& p, uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* food = coop::prop_key_index::Find(key);
    if (!food || !FD::IsFood(food)) {
        SayDenied("no food holds the key", key, senderSlot);
        return;
    }
    const EL::IntentSubject subject = EL::IntentTarget::ForClientIntent(session, senderSlot, kReachUU).Authorize(food);
    if (!subject) {
        SayDenied(EL::OutcomeName(subject.outcome), key, senderSlot);
        MarkDirty(food);   // the sender's copy is put back to the host's
        return;
    }
    FD::State st;
    if (!FD::ReadState(food, st)) {
        SayDenied("the food's state is unreadable", key, senderSlot);
        return;
    }
    if (static_cast<int32_t>(p.usesAfter) != st.uses - 1) {
        SayDenied("the count is not one under the host's", key, senderSlot);
        MarkDirty(food);
        return;
    }
    st.uses = p.usesAfter;
    if (!FD::WriteState(food, st)) {
        SayDenied("the food's state did not write", key, senderSlot);
        return;
    }
    ++g_bitesTaken;
    if (g_editSaid < 20) {
        ++g_editSaid;
        UE_LOGI("[FOOD] host: slot %u bit key='%ls' -- %d left", static_cast<unsigned>(senderSlot), key.c_str(),
                static_cast<int>(p.usesAfter));
    }
    Enroll(food);
    MarkDirty(food);
}

bool WellFormed(const coop::net::FoodStatePayload& p) {
    if (p.key.len == 0 || p.key.len > sizeof(p.key.data) || p.reserved != 0) return false;
    if ((p.flags & ~FS::kKnown) != 0) return false;
    if (!std::isfinite(p.temperature) || !std::isfinite(p.freshness) || !std::isfinite(p.cookProgress)) return false;
    if (p.temperature < 0.f || p.temperature > 5000.f || p.freshness < -1.f || p.freshness > 2.f) return false;
    if (p.cookProgress < -1.f || p.cookProgress > 100000.f) return false;
    if (p.uses < -1 || p.freshnessLeft < -1 || p.portions < -1 || p.portions > kMaxPortions) return false;
    return true;
}

void CountDropped(Drop why) { ++(why == Drop::Size ? g_dropSize : g_dropMalformed); }

void MarkDirty(void* food) {
    if (!IsHost(LoadSession()) || !food) return;
    Enroll(food);
    const auto it = g_foodIndex.find(food);
    if (it != g_foodIndex.end()) g_foods[it->second].dirty = true;
}

void OnDisconnect() {
    if (g_published || g_applied || g_cancelled || g_bitesSent || g_bitesTaken || g_bitesRefused || g_dropSize ||
        g_dropMalformed)
        UE_LOGI("[FOOD] session end -- noted=%zu published=%llu seeded=%llu applied=%llu parked=%llu cancelled=%llu "
                "bites sent=%llu taken=%llu refused=%llu not-food=%llu unread=%llu dropped size=%llu malformed=%llu",
                g_foods.size(), static_cast<unsigned long long>(g_published),
                static_cast<unsigned long long>(g_seeded), static_cast<unsigned long long>(g_applied),
                static_cast<unsigned long long>(g_parked), static_cast<unsigned long long>(g_cancelled),
                static_cast<unsigned long long>(g_bitesSent), static_cast<unsigned long long>(g_bitesTaken),
                static_cast<unsigned long long>(g_bitesRefused), static_cast<unsigned long long>(g_notFood),
                static_cast<unsigned long long>(g_unread), static_cast<unsigned long long>(g_dropSize),
                static_cast<unsigned long long>(g_dropMalformed));
    g_foods.clear();
    g_foodIndex.clear();
    g_cursor = 0;
    for (auto& q : g_seed) q.clear();
    g_parkedStates.clear();
    g_parkVersion = 0;
    g_sharedMemo.clear();
    g_published = g_seeded = g_applied = g_parked = g_cancelled = g_bitesSent = g_bitesTaken = g_bitesRefused = 0;
    g_dropSize = g_dropMalformed = g_notFood = g_unread = 0;
    g_cancelSaid = g_publishSaid = g_applySaid = g_noKeySaid = g_fullSaid = false;
    g_editSaid = 0;
    g_biteWindowMs = 0;
    g_biteWindowN = 0;
}

}  // namespace coop::props::prop_food_state
