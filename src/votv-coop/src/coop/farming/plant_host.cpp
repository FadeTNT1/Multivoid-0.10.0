// coop/farming/plant_host.cpp -- the host's half of coop/farming/plant_sync.h: the registry of named plants, the
// 1 Hz census that sends a plant's row on an edge or on drift, the Gone at its destroy, a joiner's snapshot, and the
// judge that runs a client's harvest, discard, tending, medicine and pot planting through the game's own verbs.

#include "plant_sync_detail.h"  // co-located private header (src tree, not include/)

#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_synth_key.h"

#include "ue_wrap/actors/growing_plant.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::farming::detail {
namespace {

namespace EL = coop::element;
namespace E  = ue_wrap::engine;
namespace GP = ue_wrap::growing_plant;
namespace R  = ue_wrap::reflection;
using ue_wrap::CachedObjRef;
using ue_wrap::FRotator;
using ue_wrap::FVector;

constexpr uint64_t kCensusMs = 1000;
constexpr uint64_t kFirstRowDelayMs = 1000;   // a save-loaded plant's data lands after its BeginPlay
constexpr uint64_t kDriftMs = 5000;
constexpr uint64_t kDriftSayMs = 60000;
constexpr uint64_t kBodyWaitMs = 4000;
constexpr uint64_t kHandWaitMs = 3000;
constexpr float    kReachUU = 400.0f;
// A tend's outcome was made by a held tool aimed from range (the growinator fires; the client lists plants within
// this radius around itself, plant_client_verbs kTendRadiusUU), so its reach is that radius.
constexpr float    kTendReachUU = 1500.0f;
constexpr float    kMoveUU = 5.0f;
constexpr size_t   kMaxRows = 4096;
constexpr size_t   kMaxPending = 8;
constexpr int      kSayRegistered = 16;
constexpr coop::net::IntentBudget kBudget{4.0f, 4.0f};

struct HostRow {
    CachedObjRef actor;
    std::string  key, cls, attachKey;
    GP::State    sent;
    W::Pose      pose;
    bool         announced = false;
    bool         edge = false;
    uint64_t     sentMs = 0, driftSayMs = 0;
};
std::unordered_map<std::string, HostRow> g_rows;
std::unordered_map<void*, std::string>   g_byActor;

struct Fresh {
    CachedObjRef actor;
    uint64_t     dueMs = 0;
};
std::vector<Fresh> g_fresh;

uint64_t g_nextCensusMs = 0;
bool     g_seeded = false;
bool     g_fullSaid = false;
int      g_registeredSaid = 0;
uint64_t g_registeredQuiet = 0;
uint64_t g_registered = 0, g_rowsSent = 0, g_gone = 0, g_ran = 0, g_refused = 0;

void* g_harvestPuppet = nullptr;
void* g_captured = nullptr;

// ---- naming ----

// The plant's name for the wire: its key or its level identity, or a key minted and written into it (a runtime plant
// the game has not keyed yet; the save then carries the minted key). A key another live plant already holds is
// replaced the same way, so one key names one plant.
std::string NameFor(void* plant, bool& minted) {
    minted = false;
    std::string id = IdentityOf(plant);
    if (!id.empty()) {
        auto it = g_rows.find(id);
        if (it == g_rows.end() || !it->second.actor.Alive() || it->second.actor.Is(plant)) return id;
        UE_LOGW("farm: host plant key=%s is held by two plants -- the newer gets a fresh key", id.c_str());
    }
    if (!GP::SetKey(plant, coop::prop_synth_key::RandomKeyString())) return {};
    minted = true;
    return Narrow(GP::KeyOf(plant));
}

std::string AttachKeyOf(void* plant) {
    void* parent = GP::AttachTo(plant);
    if (!parent || !ue_wrap::prop::IsKeyedInteractable(parent)) return {};
    return Narrow(ue_wrap::prop::GetInteractableKeyString(parent));
}

bool ReadPose(void* a, W::Pose& p) {
    FVector l{};
    FRotator r{};
    if (!E::TryGetActorLocation(a, l) || !E::TryGetActorRotation(a, r)) return false;
    p = W::Pose{l.X, l.Y, l.Z, r.Pitch, r.Yaw, r.Roll};
    return std::isfinite(l.X) && std::isfinite(l.Y) && std::isfinite(l.Z) && std::isfinite(r.Yaw);
}

W::Row RowOf(const HostRow& r, const GP::State& st, const W::Pose& pose, const std::string& attachKey, bool snapshot) {
    W::Row w;
    w.key = r.key;
    w.cls = r.cls;
    w.plant = Narrow(st.plant);
    w.pose = pose;
    w.attachKey = attachKey;
    w.grow = st.grow;
    w.water = st.water;
    w.dryness = st.dryness;
    w.fertilizer = st.fertilizer;
    w.fruitgrow = st.fruitgrow;
    w.health = st.health;
    w.amount = st.amount;
    w.disease = Narrow(st.disease);
    w.flags = static_cast<uint8_t>((snapshot ? W::kRowSnapshot : 0) | (st.grew ? W::kRowGrew : 0) |
                                   (st.hideDirt ? W::kRowHideDirt : 0));
    return w;
}

void SendRow(int slot, const W::Row& w) {
    W::State m;
    m.op = W::StateOp::Row;
    m.row = w;
    Enqueue(slot, W::Pack(m));
}

void SendGone(const std::string& key, bool harvested) {
    W::State m;
    m.op = W::StateOp::Gone;
    m.key = key;
    m.flags = harvested ? W::kGoneHarvested : 0;
    Enqueue(-1, W::Pack(m));
    ++g_gone;
    UE_LOGI("farm: host gone key=%s harvested=%d (%zu live)", key.c_str(), harvested ? 1 : 0, g_rows.size());
}

// Name a plant whose data has landed. False when it does not read yet (asked again at the next census). A plant with no
// row is scenery the game never grows or harvests: it is not named, and a client keeps its own.
bool Register(void* plant) {
    if (g_byActor.count(plant)) return true;
    if (g_rows.size() >= kMaxRows) {
        if (!g_fullSaid) {
            g_fullSaid = true;
            UE_LOGW("farm: host names %zu plants already -- the rest stay on the host alone", g_rows.size());
        }
        return true;
    }
    GP::State st;
    if (!GP::ReadState(plant, st)) return false;
    if (st.plant.empty()) return true;
    bool minted = false;
    std::string key = NameFor(plant, minted);
    if (key.empty() || key.size() > W::kMaxStrBytes) return false;
    HostRow r;
    r.actor.Set(plant);
    r.key = key;
    r.cls = Narrow(R::ClassNameOf(plant));
    g_byActor[plant] = key;
    g_rows[key] = std::move(r);
    ++g_registered;
    if (g_registeredSaid < kSayRegistered) {
        ++g_registeredSaid;
        UE_LOGI("farm: host registered plant key=%s%s cls=%s plant=%ls grow=%.3f (%zu live)", key.c_str(),
                minted ? " (minted)" : "", g_rows[key].cls.c_str(), st.plant.c_str(), static_cast<double>(st.grow),
                g_rows.size());
    } else {
        ++g_registeredQuiet;
    }
    return true;
}

// Why a row goes now, or null when it waits. Edges go at once; drift at most every kDriftMs.
const char* Reason(const HostRow& r, const GP::State& st, const W::Pose& pose, const std::string& attachKey, uint64_t now) {
    if (!r.announced) return "new";
    const GP::State& o = r.sent;
    if (st.plant != o.plant) return "edge:row";
    if (st.grew != o.grew) return "edge:grew";
    if ((st.dryness < 0.f) != (o.dryness < 0.f)) return "edge:dead";
    if (st.amount != o.amount) return "edge:amount";
    if (st.disease != o.disease) return "edge:disease";
    if (st.hideDirt != o.hideDirt) return "edge:dirt";
    if (attachKey != r.attachKey) return "edge:attach";
    if (attachKey.empty()) {
        const float dx = pose.x - r.pose.x, dy = pose.y - r.pose.y, dz = pose.z - r.pose.z;
        if (dx * dx + dy * dy + dz * dz > kMoveUU * kMoveUU || std::fabs(pose.yaw - r.pose.yaw) > 2.f) return "edge:move";
    }
    if (r.edge) return "edge:tend";
    if (now - r.sentMs < kDriftMs) return nullptr;
    if (std::fabs(st.grow - o.grow) >= 0.01f || (st.water > 0.f) != (o.water > 0.f) || std::fabs(st.water - o.water) >= 30.f ||
        (st.fertilizer > 0.f) != (o.fertilizer > 0.f) || std::fabs(st.fruitgrow - o.fruitgrow) >= 0.02f ||
        std::fabs(st.health - o.health) >= 1.f)
        return "drift";
    return nullptr;
}

void Census(uint64_t now, bool forceFresh) {
    if (!g_seeded) {
        g_seeded = true;
        for (void* p : GP::AllPlants()) {
            Fresh f;
            f.actor.Set(p);
            g_fresh.push_back(std::move(f));
        }
    }
    std::vector<Fresh> keep;
    for (Fresh& f : g_fresh) {
        void* a = f.actor.Get();
        if (!a) continue;
        if (!forceFresh && now < f.dueMs) { keep.push_back(std::move(f)); continue; }
        if (!Register(a)) keep.push_back(std::move(f));
    }
    g_fresh.swap(keep);
    if (g_registeredQuiet) {
        UE_LOGI("farm: host registered %llu more plants (%zu live)", static_cast<unsigned long long>(g_registeredQuiet),
                g_rows.size());
        g_registeredQuiet = 0;
    }
    for (auto it = g_rows.begin(); it != g_rows.end();) {
        HostRow& r = it->second;
        void* a = r.actor.Get();
        if (!a) {
            const std::string key = it->first;
            g_byActor.erase(r.actor.Raw());
            it = g_rows.erase(it);
            SendGone(key, false);
            continue;
        }
        GP::State st;
        W::Pose pose;
        if (!GP::ReadState(a, st) || !ReadPose(a, pose)) { ++it; continue; }
        const std::string attachKey = AttachKeyOf(a);
        const char* why = Reason(r, st, pose, attachKey, now);
        if (!why) { ++it; continue; }
        const bool drift = why[0] == 'd';
        r.sent = st;
        r.pose = pose;
        r.attachKey = attachKey;
        r.announced = true;
        r.edge = false;
        r.sentMs = now;
        SendRow(-1, RowOf(r, st, pose, attachKey, false));
        ++g_rowsSent;
        if (!drift || now >= r.driftSayMs) {
            if (drift) r.driftSayMs = now + kDriftSayMs;
            UE_LOGI("farm: host row -> all key=%s reason=%s plant=%ls grow=%.3f water=%.0f dry=%.2f fert=%.0f fruit=%.2f "
                    "amount=%d disease=%ls", r.key.c_str(), why, st.plant.c_str(), static_cast<double>(st.grow),
                    static_cast<double>(st.water), static_cast<double>(st.dryness), static_cast<double>(st.fertilizer),
                    static_cast<double>(st.fruitgrow), st.amount, st.disease.empty() ? L"none" : st.disease.c_str());
        }
        ++it;
    }
}

// ---- the judge ----

struct Queued {
    W::Intent m;
    uint64_t  atMs = 0;
    uint64_t  handSinceMs = 0;
};
struct Slot {
    coop::net::IntentQueue<Queued> queue;
};
Slot g_slots[coop::net::kMaxPeers];

const char* OpName(W::IntentOp op) {
    switch (op) {
        case W::IntentOp::Harvest: return "Harvest";
        case W::IntentOp::Discard: return "Discard";
        case W::IntentOp::Tend: return "Tend";
        case W::IntentOp::Medicine: return "Medicine";
        case W::IntentOp::PotPlant: return "PotPlant";
    }
    return "?";
}

void Answer(uint8_t slot, const W::Intent& m, bool ok, const char* why, const std::string& itemKey = {}) {
    W::State a;
    a.op = W::StateOp::Ack;
    a.reqId = m.reqId;
    a.ok = ok ? 1 : 0;
    a.itemKey = itemKey;
    Enqueue(slot, W::Pack(a));
    ok ? ++g_ran : ++g_refused;
    if (ok)
        UE_LOGI("farm: host intent slot=%u op=%s key=%s req=%u -> ok%s%s", static_cast<unsigned>(slot), OpName(m.op),
                m.key.c_str(), m.reqId, itemKey.empty() ? "" : " item=", itemKey.c_str());
    else
        UE_LOGI("farm: host intent slot=%u op=%s key=%s req=%u -> refused(%s)", static_cast<unsigned>(slot), OpName(m.op),
                m.key.c_str(), m.reqId, why);
}

void* PuppetBody(uint8_t slot) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    return (rp && rp->valid()) ? rp->GetActor() : nullptr;
}

void MarkEdge(void* plant) {
    auto it = g_byActor.find(plant);
    if (it == g_byActor.end()) return;
    auto r = g_rows.find(it->second);
    if (r != g_rows.end()) r->second.edge = true;
}

// Raise one tended field toward the client's value, never lower it, within the cap the game's own tools reach.
bool Raise(float& field, float wire, float cap) {
    if (wire <= field) return false;
    field = std::min(wire, cap);
    return true;
}

void RunPlantOp(const Queued& q, uint8_t slot, void* plant, void* body) {
    const W::Intent& m = q.m;
    GP::State st;
    if (!GP::ReadState(plant, st)) { Answer(slot, m, false, "the plant's state does not read"); return; }
    switch (m.op) {
        case W::IntentOp::Harvest: {
            if (st.plantType == 2) { Answer(slot, m, false, "a tree has no harvest"); return; }
            if (st.plantType == 1 && st.amount <= 0) { Answer(slot, m, false, "no berries on the bush"); return; }
            GP::WriteDiscard(plant, false);
            g_harvestPuppet = body;
            g_captured = nullptr;
            const bool ran = GP::CallHarvestVerb(plant, body);
            g_harvestPuppet = nullptr;
            void* item = g_captured;
            g_captured = nullptr;
            if (!ran) { Answer(slot, m, false, "the host's copy did not take the call"); return; }
            std::string itemKey;
            if (item && R::IsLive(item)) {
                itemKey = Narrow(ue_wrap::prop::GetInteractableKeyString(item));
                UE_LOGI("farm: host harvest captured item key=%s for slot %u", itemKey.c_str(), static_cast<unsigned>(slot));
            }
            MarkEdge(plant);
            Answer(slot, m, true, nullptr, itemKey);
            return;
        }
        case W::IntentOp::Discard:
            if (st.grow >= 1.f && !(st.amountMin <= 0 && st.amountMax <= 0)) {
                Answer(slot, m, false, "a grown plant with produce is harvested, not discarded");
                return;
            }
            E::DestroyActor(plant);
            Answer(slot, m, true, nullptr);
            return;
        case W::IntentOp::Tend: {
            bool moved = false;
            if (m.mask & W::kTendWater) moved |= Raise(st.water, m.water, 3600.f);
            if (m.mask & W::kTendFertilizer) moved |= Raise(st.fertilizer, m.fertilizer, 1200.f);
            if (m.mask & W::kTendGrow) moved |= Raise(st.grow, m.grow, 0.999999f);
            if (m.mask & W::kTendFruit) moved |= Raise(st.fruitgrow, m.fruitgrow, 0.999999f);
            if (moved) {
                GP::State write = st;
                write.plant.clear();   // the row is the plant's own
                GP::WriteState(plant, write);
                GP::CallUpdLoc(plant);
                MarkEdge(plant);
            }
            Answer(slot, m, true, nullptr);
            return;
        }
        case W::IntentOp::Medicine:
            if (!(m.time > 0.f && m.time <= 30.f) || m.type < 1 || m.type > 9) {
                Answer(slot, m, false, "the medicine is out of range");
                return;
            }
            GP::AddMedicine(plant, m.type, m.time);
            Answer(slot, m, true, nullptr);
            return;
        case W::IntentOp::PotPlant: break;
    }
}

// False keeps the request at the head of its sender's queue: the host has no body or no hand for it yet.
bool RunPotPlant(Queued& q, uint8_t slot, const EL::IntentTarget& reach, void* body) {
    const W::Intent& m = q.m;
    void* pot = coop::prop_key_index::Find(Widen(m.key));
    if (!pot || !GP::IsFarmPot(pot)) { Answer(slot, m, false, "no pot holds the key"); return true; }
    const EL::IntentSubject subject = reach.Authorize(pot);
    if (!subject) { Answer(slot, m, false, EL::OutcomeName(subject.outcome)); return true; }
    const std::wstring seedCls = Widen(m.seedCls), seedName = Widen(m.seedName);
    if (!coop::hand_item::HeldClassIs(slot, seedCls.c_str())) {
        const uint64_t now = NowMs();
        if (q.handSinceMs == 0) q.handSinceMs = now;
        if (now - q.handSinceMs < kHandWaitMs) return false;   // the hand state may still be on its way
        Answer(slot, m, false, "the sender's hand shows no such item");
        return true;
    }
    if (!GP::PotWouldPlant(pot, seedName)) { Answer(slot, m, false, "the pot would not plant this item"); return true; }
    void* cls = ue_wrap::object_index::ClassByName(seedCls.c_str());
    FVector at{};
    E::TryGetActorLocation(pot, at);
    at.Z += 40.f;
    void* seed = nullptr;
    {
        coop::prop_echo_suppress::ScopedMirrorSpawn guard;
        seed = GP::SpawnSeed(cls, at, seedName);
        if (seed) coop::prop_echo_suppress::MarkMirrorSpawn(seed);
    }
    if (!seed) { Answer(slot, m, false, "the seed did not rebuild"); return true; }
    coop::prop_echo_suppress::MarkIncomingDestroy(seed);
    GP::CallPotPlant(pot, body, seed, seedName);
    if (R::IsLive(seed)) E::DestroyActor(seed);   // a refused seed: the body did not spend it
    if (!GP::PotPlant(pot)) { Answer(slot, m, false, "the pot's own playerUsedOn planted nothing"); return true; }
    Answer(slot, m, true, nullptr);   // the new plant's BeginPlay names it, and its row follows
    return true;
}

coop::net::Served Execute(coop::net::Session& s, Queued& q, uint8_t slot) {
    using coop::net::Served;
    const W::Intent& m = q.m;
    if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) {
        Answer(slot, m, false, "the sender's player is not alive");
        return Served::Done;
    }
    const EL::IntentTarget reach =
        EL::IntentTarget::ForClientIntent(s, slot, m.op == W::IntentOp::Tend ? kTendReachUU : kReachUU);
    void* body = PuppetBody(slot);
    if (!reach.HasBody() || !body) {
        if (NowMs() - q.atMs < kBodyWaitMs) return Served::Wait;
        Answer(slot, m, false, "the host has no body for the sender");
        return Served::Done;
    }
    if (m.op == W::IntentOp::PotPlant) return RunPotPlant(q, slot, reach, body) ? Served::Done : Served::Wait;
    auto it = g_rows.find(m.key);
    void* plant = it != g_rows.end() ? it->second.actor.Get() : nullptr;
    if (!plant) { Answer(slot, m, false, "no plant holds the key"); return Served::Done; }
    const EL::IntentSubject subject = reach.Authorize(plant);
    if (!subject) { Answer(slot, m, false, EL::OutcomeName(subject.outcome)); return Served::Done; }
    RunPlantOp(q, slot, plant, body);
    return Served::Done;
}

}  // namespace

void HostTick(coop::net::Session& s) {
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot)
        g_slots[slot].queue.Serve(slot, kBudget, NowMs, [&](Queued& q) { return Execute(s, q, slot); });
    const uint64_t now = NowMs();
    if (now < g_nextCensusMs) return;
    g_nextCensusMs = now + kCensusMs;
    Census(now, false);
}

void HostOnBeginPlay(void* plant) {
    if (g_byActor.count(plant)) return;
    Fresh f;
    f.actor.Set(plant);
    f.dueMs = NowMs() + kFirstRowDelayMs;
    g_fresh.push_back(std::move(f));
}

void HostOnDestroyed(void* plant, bool harvested) {
    auto it = g_byActor.find(plant);
    if (it == g_byActor.end()) return;
    const std::string key = it->second;
    g_byActor.erase(it);
    g_rows.erase(key);
    SendGone(key, harvested);
}

void HostOnIntent(W::Intent&& m, uint8_t slot) {
    Queued q;
    q.m = std::move(m);
    q.atMs = NowMs();
    UE_LOGI("farm: host received slot %u's intent op=%s key=%s req=%u", static_cast<unsigned>(slot), OpName(q.m.op),
            q.m.key.c_str(), q.m.reqId);
    const W::Intent copy = q.m;
    if (!g_slots[slot].queue.Push(std::move(q), kMaxPending)) Answer(slot, copy, false, "queue full");
}

void HostSnapshotTo(int slot) {
    Census(NowMs(), true);
    int sent = 0;
    for (auto& [key, r] : g_rows) {
        void* a = r.actor.Get();
        GP::State st;
        W::Pose pose;
        if (!a || !GP::ReadState(a, st) || !ReadPose(a, pose)) continue;
        SendRow(slot, RowOf(r, st, pose, AttachKeyOf(a), true));
        ++sent;
    }
    W::State end;
    end.op = W::StateOp::SnapshotEnd;
    Enqueue(slot, W::Pack(end));
    UE_LOGI("farm: host snapshot -> slot %d rows=%d", slot, sent);
}

void HostNoteWatered(void* plant) { MarkEdge(plant); }

void* HostHarvestPuppet() { return g_harvestPuppet; }
void HostCaptureItem(void* item) { g_captured = item; }

void HostPeerLeft(uint8_t slot) {
    if (slot < coop::net::kMaxPeers) g_slots[slot] = Slot{};
}

void HostReset() {
    if (g_registered || g_ran || g_refused)
        UE_LOGI("farm: session end (host) -- named %llu plants, %llu rows sent, %llu gone, intents ran %llu refused %llu",
                static_cast<unsigned long long>(g_registered), static_cast<unsigned long long>(g_rowsSent),
                static_cast<unsigned long long>(g_gone), static_cast<unsigned long long>(g_ran),
                static_cast<unsigned long long>(g_refused));
    g_rows.clear();
    g_byActor.clear();
    g_fresh.clear();
    for (auto& sl : g_slots) sl = Slot{};
    g_nextCensusMs = 0;
    g_seeded = g_fullSaid = false;
    g_registeredSaid = 0;
    g_registeredQuiet = 0;
    g_registered = g_rowsSent = g_gone = g_ran = g_refused = 0;
    g_harvestPuppet = g_captured = nullptr;
}

}  // namespace coop::farming::detail
