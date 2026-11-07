// coop/farming/plant_client.cpp -- the client's half of coop/farming/plant_sync.h: the host's rows bound to this peer's
// plants (by key, else by class and place, else a spawned mirror), the mirrored state applied through the plant's own
// upd, updLoc and processGrown, the parked tick, Gone, the snapshot's end, and the answers to this client's requests.

#include "plant_sync_detail.h"  // co-located private header (src tree, not include/)

#include "coop/player/players_registry.h"
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/prop_key_index.h"

#include "ue_wrap/actors/growing_plant.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <cmath>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace coop::farming::detail {
namespace {

namespace E  = ue_wrap::engine;
namespace GP = ue_wrap::growing_plant;
namespace R  = ue_wrap::reflection;
using ue_wrap::CachedObjRef;
using ue_wrap::FRotator;
using ue_wrap::FVector;

constexpr float    kAdoptUU = 6.0f;      // a plant of the row's class this near the row's place is the row's
constexpr float    kMoveUU = 5.0f;
constexpr uint64_t kIndexMs = 500;       // this peer's plants are listed again on a miss at most this often
constexpr uint64_t kParkRetryMs = 500;
constexpr uint64_t kParkMs = 30000;
constexpr uint64_t kAckWaitMs = 10000;
constexpr uint64_t kPocketWaitMs = 5000;
constexpr uint64_t kPocketTryMs = 250;
constexpr uint64_t kAppliedSayMs = 30000;
constexpr int      kSayBound = 16;

struct Bound {
    CachedObjRef actor;
    uint64_t     sayMs = 0;
};
std::unordered_map<std::string, Bound> g_bound;
std::unordered_map<void*, std::string> g_byActor;

struct Parked {
    W::Row   row;
    uint64_t sinceMs = 0;
};
std::vector<Parked> g_parked;
uint64_t g_nextRetryMs = 0;

// This peer's own plants by their cross-peer name, and every one of them for the place match.
struct Local {
    CachedObjRef actor;
    std::string  id, cls;
    FVector      loc{};
};
std::vector<Local> g_index;
uint64_t g_indexAtMs = 0;
bool     g_indexBuilt = false;

bool g_snapOpen = false;
std::unordered_set<std::string> g_snapNamed;

struct PendingReq {
    Pending      what = Pending::Harvest;
    std::string  key;
    CachedObjRef spent;
    uint64_t     untilMs = 0;
};
std::unordered_map<uint32_t, PendingReq> g_pending;
uint32_t g_nextReq = 1;

struct Pocket {
    std::string itemKey;
    uint64_t    untilMs = 0;
    uint64_t    nextTryMs = 0;
};
std::vector<Pocket> g_pockets;

int      g_mirrorSpawning = 0;
int      g_boundSaid = 0;
uint64_t g_adopted = 0, g_spawned = 0, g_applied = 0, g_goneApplied = 0, g_sent = 0, g_pocketed = 0;

const char* WhatName(Pending w) {
    switch (w) {
        case Pending::Harvest: return "Harvest";
        case Pending::Discard: return "Discard";
        case Pending::Tend: return "Tend";
        case Pending::Medicine: return "Medicine";
        case Pending::PotPlant: return "PotPlant";
    }
    return "?";
}

void BuildIndex(uint64_t now) {
    g_index.clear();
    for (void* a : GP::AllPlants()) {
        Local l;
        l.actor.Set(a);
        l.id = IdentityOf(a);
        l.cls = Narrow(R::ClassNameOf(a));
        E::TryGetActorLocation(a, l.loc);
        g_index.push_back(std::move(l));
    }
    g_indexAtMs = now;
    g_indexBuilt = true;
}

}  // namespace

void ClientOnBeginPlay(void* plant) {
    if (!g_indexBuilt || !plant) return;   // the next build lists it
    Local l;
    l.actor.Set(plant);
    l.id = IdentityOf(plant);
    l.cls = Narrow(R::ClassNameOf(plant));
    E::TryGetActorLocation(plant, l.loc);
    g_index.push_back(std::move(l));
}

namespace {

void* FindLocal(const W::Row& row, bool& byPlace) {
    byPlace = false;
    const uint64_t now = NowMs();
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1 || !g_indexBuilt) {
            if (g_indexBuilt && now - g_indexAtMs < kIndexMs) return nullptr;
            BuildIndex(now);
        }
        for (const Local& l : g_index) {
            void* a = l.actor.Get();
            if (a && l.id == row.key && !g_byActor.count(a)) return a;
        }
        const float lim = kAdoptUU * kAdoptUU;
        for (const Local& l : g_index) {
            void* a = l.actor.Get();
            if (!a || g_byActor.count(a) || l.cls != row.cls) continue;
            const float dx = l.loc.X - row.pose.x, dy = l.loc.Y - row.pose.y, dz = l.loc.Z - row.pose.z;
            if (dx * dx + dy * dy + dz * dz <= lim) {
                byPlace = true;
                return a;
            }
        }
    }
    return nullptr;
}

// A key the host minted is the game's own key field; a level identity ("mv_") is ours and never written into the game.
bool IsGameKey(const std::string& key) { return key.rfind("mv_", 0) != 0; }

void* SpawnMirror(const W::Row& row, bool& parked) {
    parked = false;
    void* cls = ue_wrap::object_index::ClassByName(Widen(row.cls).c_str());
    if (!cls || !GP::IsPlantClass(cls)) return nullptr;
    void* parent = nullptr;
    if (!row.attachKey.empty()) {
        parent = coop::prop_key_index::Find(Widen(row.attachKey));
        if (!parent) {
            parked = true;
            return nullptr;
        }
    }
    void* a = nullptr;
    ++g_mirrorSpawning;
    {
        coop::prop_echo_suppress::ScopedMirrorSpawn guard;
        a = GP::SpawnPlant(cls, FVector{row.pose.x, row.pose.y, row.pose.z}, FRotator{row.pose.pitch, row.pose.yaw, row.pose.roll},
                           Widen(row.plant), IsGameKey(row.key) ? Widen(row.key) : std::wstring{}, parent,
                           (row.flags & W::kRowHideDirt) != 0);
        if (a) coop::prop_echo_suppress::MarkMirrorSpawn(a);
    }
    --g_mirrorSpawning;
    return a;
}

// The host's state into this peer's plant, through the plant's own verbs, then its tick parked. The parking removes a
// second author: the growth tick (refused besides) would roll its own sun, rain and growth over the host's row.
void ApplyState(void* a, const W::Row& row, Bound& b, bool fresh) {
    GP::State cur;
    if (!GP::ReadState(a, cur)) return;
    GP::State want = cur;
    want.plant = Widen(row.plant);
    want.grow = row.grow;
    want.water = row.water;
    want.dryness = row.dryness;
    want.fertilizer = row.fertilizer;
    want.fruitgrow = row.fruitgrow;
    want.health = row.health;
    want.amount = row.amount;
    want.hideDirt = (row.flags & W::kRowHideDirt) != 0;
    const bool rowChanged = cur.plant != want.plant;
    GP::WriteState(a, want);
    GP::CallUpd(a, false);
    GP::CallUpdLoc(a);
    const bool ripen = want.grow >= 1.f && (!cur.grew || rowChanged);
    if (ripen) {
        if (GP::IsStolasHerb(a)) GP::WriteGrew(a, true);   // its grown() strikes lightning and destroys it
        else GP::CallProcessGrown(a, true);
    }
    const std::wstring disease = Widen(row.disease);
    const bool diseaseChanged = disease != cur.disease;
    if (diseaseChanged) {
        if (!cur.disease.empty()) GP::HealDisease(a);
        bool ok = false;
        if (!disease.empty()) GP::AddDisease(a, disease, ok);
    }
    if (row.attachKey.empty() && !GP::AttachTo(a)) {
        FVector l{};
        if (E::TryGetActorLocation(a, l)) {
            const float dx = l.X - row.pose.x, dy = l.Y - row.pose.y, dz = l.Z - row.pose.z;
            if (dx * dx + dy * dy + dz * dz > kMoveUU * kMoveUU) {
                E::SetActorLocation(a, FVector{row.pose.x, row.pose.y, row.pose.z});
                E::SetActorRotation(a, FRotator{row.pose.pitch, row.pose.yaw, row.pose.roll});
            }
        }
    }
    E::SetActorTickEnabled(a, false);
    ++g_applied;
    const uint64_t now = NowMs();
    if (fresh || ripen || rowChanged || diseaseChanged || now >= b.sayMs) {
        b.sayMs = now + kAppliedSayMs;
        if (!fresh)
            UE_LOGI("farm: client row key=%s applied plant=%s grow=%.3f water=%.0f dry=%.2f amount=%d disease=%s%s",
                    row.key.c_str(), row.plant.c_str(), static_cast<double>(row.grow), static_cast<double>(row.water),
                    static_cast<double>(row.dryness), row.amount, row.disease.empty() ? "none" : row.disease.c_str(),
                    ripen ? " (ripened)" : "");
    }
}

// True when the row is dealt with (bound and applied); false when it waits for its pot.
bool ApplyRow(const W::Row& row) {
    if (row.flags & W::kRowSnapshot) {
        if (!g_snapOpen) {
            g_snapOpen = true;
            g_snapNamed.clear();
        }
        g_snapNamed.insert(row.key);
    }
    auto it = g_bound.find(row.key);
    void* a = it != g_bound.end() ? it->second.actor.Get() : nullptr;
    if (a) {
        ApplyState(a, row, it->second, false);
        return true;
    }
    if (it != g_bound.end()) {
        g_byActor.erase(it->second.actor.Raw());
        g_bound.erase(it);
    }
    bool byPlace = false, parked = false;
    a = FindLocal(row, byPlace);
    const bool adopted = a != nullptr;
    if (!a) a = SpawnMirror(row, parked);
    if (!a) return !parked;   // a class this peer cannot spawn is left out; a missing pot waits
    if (adopted && byPlace && IsGameKey(row.key) && IdentityOf(a) != row.key) GP::SetKey(a, Widen(row.key));
    Bound& b = g_bound[row.key];
    b.actor.Set(a);
    g_byActor[a] = row.key;
    adopted ? ++g_adopted : ++g_spawned;
    if (g_boundSaid < kSayBound) {
        ++g_boundSaid;
        UE_LOGI("farm: client row key=%s %s cls=%s plant=%s grow=%.3f water=%.0f", row.key.c_str(),
                adopted ? (byPlace ? "adopted (by place)" : "adopted") : "spawned", row.cls.c_str(), row.plant.c_str(),
                static_cast<double>(row.grow), static_cast<double>(row.water));
    }
    ApplyState(a, row, b, true);
    return true;
}

void ApplyGone(const W::State& m) {
    for (auto p = g_parked.begin(); p != g_parked.end();) p = p->row.key == m.key ? g_parked.erase(p) : std::next(p);
    auto it = g_bound.find(m.key);
    if (it == g_bound.end()) return;
    if (void* a = it->second.actor.Get()) {
        if (m.flags & W::kGoneHarvested) GP::PlayHarvestSound(a);
        coop::prop_echo_suppress::MarkIncomingDestroy(a);
        E::DestroyActor(a);   // its destroy body is refused here: the host's drops arrive as props
    }
    g_byActor.erase(it->second.actor.Raw());
    g_bound.erase(it);
    ++g_goneApplied;
    UE_LOGI("farm: client gone key=%s harvested=%d", m.key.c_str(), (m.flags & W::kGoneHarvested) ? 1 : 0);
}

// The host named every plant it has: this peer's plants it did not name are this peer's own drift from the save.
void ApplySnapshotEnd() {
    int named = static_cast<int>(g_snapNamed.size()), destroyed = 0;
    for (void* a : GP::AllPlants()) {
        GP::State st;
        if (g_byActor.count(a) || !GP::ReadState(a, st) || st.plant.empty()) continue;   // a rowless plant is scenery
        const std::string id = IdentityOf(a);
        if (!id.empty() && g_snapNamed.count(id)) continue;
        coop::prop_echo_suppress::MarkIncomingDestroy(a);
        E::DestroyActor(a);
        ++destroyed;
    }
    g_snapOpen = false;
    g_snapNamed.clear();
    g_indexBuilt = false;
    UE_LOGI("farm: client snapshot end: named=%d bound=%zu (adopted %llu, spawned %llu) parked=%zu destroyed-unnamed=%d",
            named, g_bound.size(), static_cast<unsigned long long>(g_adopted), static_cast<unsigned long long>(g_spawned),
            g_parked.size(), destroyed);
}

void ApplyAck(const W::State& m) {
    auto it = g_pending.find(m.reqId);
    if (it == g_pending.end()) return;
    PendingReq p = std::move(it->second);
    g_pending.erase(it);
    UE_LOGI("farm: client ack reqId=%u op=%s ok=%u item=%s", m.reqId, WhatName(p.what), static_cast<unsigned>(m.ok),
            m.itemKey.empty() ? "-" : m.itemKey.c_str());
    if (!m.ok) return;
    if (p.what == Pending::PotPlant) {
        // What the pot's own body does with the planted item: it leaves the hand.
        if (void* spent = p.spent.Get()) E::DestroyActor(spent);
        return;
    }
    if (p.what == Pending::Harvest && !m.itemKey.empty()) {
        auto b = g_bound.find(p.key);
        if (b != g_bound.end())
            if (void* a = b->second.actor.Get()) GP::PlayHarvestSound(a);
        g_pockets.push_back(Pocket{m.itemKey, NowMs() + kPocketWaitMs, 0});
    }
}

// The berry the host's harvest left in the world for this player, pocketed through the player's own verb once its prop
// has arrived here: the pocket lane then asks the host as for any pickup.
void PumpPockets(uint64_t now) {
    for (auto it = g_pockets.begin(); it != g_pockets.end();) {
        if (now > it->untilMs) {
            UE_LOGW("farm: client harvested item key=%s did not arrive in %llu s -- it stays in the world",
                    it->itemKey.c_str(), static_cast<unsigned long long>(kPocketWaitMs / 1000));
            it = g_pockets.erase(it);
            continue;
        }
        if (now < it->nextTryMs) { ++it; continue; }
        it->nextTryMs = now + kPocketTryMs;
        void* item = coop::prop_key_index::Find(Widen(it->itemKey));
        void* local = coop::players::Registry::Get().Local();
        if (!item || !local) { ++it; continue; }
        bool success = false;
        GP::CallRetargetAddToInventory(local, item, success);
        ++g_pocketed;
        UE_LOGI("farm: client pocketed harvested item key=%s (asked: %s)", it->itemKey.c_str(), success ? "taken" : "sent");
        it = g_pockets.erase(it);
    }
}

}  // namespace

void ClientApply(const W::State& m) {
    switch (m.op) {
        case W::StateOp::Row:
            if (!ApplyRow(m.row)) {
                for (Parked& p : g_parked)
                    if (p.row.key == m.row.key) { p.row = m.row; return; }
                g_parked.push_back(Parked{m.row, NowMs()});
                UE_LOGI("farm: client parked row key=%s (attach unresolved: %s)", m.row.key.c_str(), m.row.attachKey.c_str());
            }
            return;
        case W::StateOp::Gone: ApplyGone(m); return;
        case W::StateOp::SnapshotEnd: ApplySnapshotEnd(); return;
        case W::StateOp::Ack: ApplyAck(m); return;
    }
}

void ClientTick() {
    const uint64_t now = NowMs();
    for (auto it = g_pending.begin(); it != g_pending.end();) it = now > it->second.untilMs ? g_pending.erase(it) : std::next(it);
    if (!g_pockets.empty()) PumpPockets(now);
    if (g_parked.empty() || now < g_nextRetryMs) return;
    g_nextRetryMs = now + kParkRetryMs;
    std::vector<Parked> keep;
    for (Parked& p : g_parked) {
        if (now - p.sinceMs > kParkMs) {
            UE_LOGW("farm: client row key=%s waited %llu s for its pot key=%s -- left out", p.row.key.c_str(),
                    static_cast<unsigned long long>(kParkMs / 1000), p.row.attachKey.c_str());
            continue;
        }
        if (!ApplyRow(p.row)) keep.push_back(std::move(p));
    }
    g_parked.swap(keep);
}

std::string ClientKeyOf(void* plant) {
    auto it = g_byActor.find(plant);
    if (it == g_byActor.end()) return {};
    auto b = g_bound.find(it->second);
    return (b != g_bound.end() && b->second.actor.Is(plant) && b->second.actor.Alive()) ? it->second : std::string{};
}

std::vector<Near> ClientBoundNear(const float at[3], float radiusUU) {
    std::vector<Near> out;
    const float lim = radiusUU * radiusUU;
    for (auto& [key, b] : g_bound) {
        void* a = b.actor.Get();
        FVector l{};
        if (!a || !E::TryGetActorLocation(a, l)) continue;
        const float dx = l.X - at[0], dy = l.Y - at[1], dz = l.Z - at[2];
        if (dx * dx + dy * dy + dz * dz <= lim) out.push_back(Near{a, key});
    }
    return out;
}

bool ClientMirrorSpawning() { return g_mirrorSpawning > 0; }

uint32_t ClientSend(W::Intent&& m, Pending what, void* spentItem) {
    m.reqId = g_nextReq++;
    PendingReq p;
    p.what = what;
    p.key = m.key;
    if (spentItem) p.spent.Set(spentItem);
    p.untilMs = NowMs() + kAckWaitMs;
    g_pending[m.reqId] = std::move(p);
    ++g_sent;
    UE_LOGI("farm: client intent op=%s key=%s reqId=%u", WhatName(what), m.key.c_str(), m.reqId);
    const uint32_t req = m.reqId;
    Enqueue(-1, W::Pack(m));
    return req;
}

void ClientReset() {
    // The parked plants tick again as the game's own: tick on while alive, as upd sets it.
    int restored = 0;
    for (auto& [key, b] : g_bound) {
        void* a = b.actor.Get();
        GP::State st;
        if (!a || !GP::ReadState(a, st)) continue;
        E::SetActorTickEnabled(a, st.dryness >= 0.f);
        ++restored;
    }
    if (!g_bound.empty() || g_sent)
        UE_LOGI("farm: session end (client) -- %d plants tick again; adopted %llu spawned %llu, rows %llu, gone %llu, "
                "requests %llu, pocketed %llu", restored, static_cast<unsigned long long>(g_adopted),
                static_cast<unsigned long long>(g_spawned), static_cast<unsigned long long>(g_applied),
                static_cast<unsigned long long>(g_goneApplied), static_cast<unsigned long long>(g_sent),
                static_cast<unsigned long long>(g_pocketed));
    g_bound.clear();
    g_byActor.clear();
    g_parked.clear();
    g_index.clear();
    g_indexBuilt = false;
    g_snapOpen = false;
    g_snapNamed.clear();
    g_pending.clear();
    g_pockets.clear();
    g_nextReq = 1;
    g_nextRetryMs = 0;
    g_mirrorSpawning = 0;
    g_boundSaid = 0;
    g_adopted = g_spawned = g_applied = g_goneApplied = g_sent = g_pocketed = 0;
}

}  // namespace coop::farming::detail
