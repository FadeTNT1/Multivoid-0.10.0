// coop/props/dig_sync.cpp -- see coop/props/dig_sync.h.

#include "coop/props/dig_sync.h"

#include "coop/props/dig_wire.h"

#include "coop/element/intent_authority.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/intent_queue.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_echo_suppress.h"

#include "ue_wrap/actors/dirt.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace coop::dig_sync {
namespace {

namespace EL = coop::element;
namespace D  = ue_wrap::dirt;
namespace E  = ue_wrap::engine;
namespace N  = ue_wrap::profile::name;
namespace R  = ue_wrap::reflection;
namespace W  = coop::dig_wire;
namespace sg = ue_wrap::script_gate;
using ue_wrap::CachedObjRef;
using ue_wrap::FRotator;
using ue_wrap::FVector;

UE_FACT_FN(kRowHoleBeginPlay, "dig_sync", L"dirthole_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowHoleUse, "dig_sync", L"dirthole_C", L"playerUsedOn", Declared);
UE_FACT_FN(kRowHoleOverlap, "dig_sync", L"dirthole_C",
           L"bndevt__Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowShovelDig, "dig_sync", L"prop_shovel_C", L"event_dighole", Declared);
UE_FACT_FN(kRowBury, "dig_sync", L"dirthole_C", L"processBuriedItem", Declared);
UE_FACT_FN(kRowPlantBeginPlay, "dig_sync", L"growingPlant_C", L"ReceiveBeginPlay", Declared);

constexpr int kTagHoleBeginPlay = 0x44494731;   // 'DIG1'
constexpr int kTagHoleUse = 0x44494732;
constexpr int kTagHoleOverlap = 0x44494733;
constexpr int kTagShovelDig = 0x44494734;
constexpr int kTagBury = 0x44494735;
constexpr int kTagPlantBeginPlay = 0x44494736;

struct Watch {
    const ue_wrap::facts::Function* row;
    int                             tag;
    const char*                     what;
    sg::PreFn                       pre;
    sg::PostFn                      post;
};

constexpr float    kReachUU = 650.0f;          // the digger's body to the hole (the tool's arm and the pose staleness)
constexpr float    kAdoptUU = 6.0f;            // a native actor this near a row's place is the row's
constexpr float    kSameHoleUU = 25.0f;        // a second dig this near a hole is the first
constexpr float    kBuryNearUU = 60.0f;        // the hole a burial names, to the one the host has
constexpr size_t   kMaxRows = 512;
constexpr size_t   kMaxPendingIntents = 8;
constexpr size_t   kMaxQueuedOut = 256;
constexpr uint64_t kCensusMs = 1000;
constexpr uint64_t kBodyWaitMs = 4000;
constexpr uint64_t kAckWaitMs = 10000;
constexpr uint64_t kParkMs = 30000;
constexpr coop::net::IntentBudget kBudget{4.0f, 2.0f};

std::atomic<coop::net::Session*> g_session{nullptr};

bool IsClient(coop::net::Session* s) {
    return s && s->running() && s->connected() && s->role() == coop::net::Role::Client;
}
bool IsHost(coop::net::Session* s) { return s && s->running() && s->role() == coop::net::Role::Host; }
uint64_t NowMs() { return coop::net::NowMs(); }

std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back((c >= 0x20 && c <= 0x7e) ? static_cast<char>(c) : '?');
    return s;
}
std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

W::Pose PoseOf(const FVector& l, const FRotator& r) { return W::Pose{l.X, l.Y, l.Z, r.Pitch, r.Yaw, r.Roll}; }
FVector LocOf(const W::Pose& p) { return FVector{p.x, p.y, p.z}; }
FRotator RotOf(const W::Pose& p) { return FRotator{p.pitch, p.yaw, p.roll}; }
float Dist2(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz;
}

// ---- the outbound queue: one for the host's state, one for a client's intents; a send the link refuses stays ----

struct Out {
    int                  slot;   // -1: every client (state); a client's intents go to the host
    std::vector<uint8_t> bytes;
};
std::deque<Out> g_out;
uint32_t g_sendSeq = 1;
bool g_fullSaid = false;

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

void DrainOut(coop::net::Session& s) {
    if (!s.running() || !s.connected()) return;
    const bool host = s.role() == coop::net::Role::Host;
    const auto kind = host ? coop::net::ReliableKind::DigState : coop::net::ReliableKind::DigIntent;
    for (int n = 0; n < 16 && !g_out.empty(); ++n) {
        const Out& o = g_out.front();
        const bool ok = (host && o.slot >= 0)
                            ? coop::blob_chunks::SendBlobToSlot(&s, o.slot, kind, g_sendSeq, o.bytes)
                            : coop::blob_chunks::SendBlob(&s, kind, g_sendSeq, o.bytes);
        if (!ok) return;   // the blob is all or nothing: the whole of it again next tick, under a fresh number
        ++g_sendSeq;
        g_out.pop_front();
    }
}

// ---- state shared by the gate callbacks ----

int  g_digDepth = 0;          // inside a shovel's event_dighole body
int  g_buryDepth = 0;         // HOST: inside a hole's processBuriedItem body
int  g_mirrorSpawning = 0;    // CLIENT: inside our own spawn of a mirror
std::vector<CachedObjRef> g_capturedPlants;   // HOST: plants that began play inside a burial

// ---- HOST ----

enum class Kind : uint8_t { Hole, Mound, Plant };

struct HostRow {
    uint32_t             id = 0;
    Kind                 kind = Kind::Hole;
    CachedObjRef         actor;
    std::string          cls;
    FVector              loc{};
    FRotator             rot{};
    int32_t              mat = 0;
    std::string          plant;
    std::vector<uint8_t> blob;
};
std::unordered_map<uint32_t, HostRow> g_host;
std::unordered_map<void*, uint32_t>   g_hostByActor;
uint32_t g_nextId = 1;
uint64_t g_nextCensus = 0;
bool g_rowsFullSaid = false;
uint64_t g_registered = 0, g_gone = 0, g_digsRun = 0, g_buriesRun = 0, g_fillsRun = 0, g_denied = 0, g_dropped = 0;

W::State StateOf(const HostRow& r, bool snapshot) {
    W::State m;
    m.id = r.id;
    m.cls = r.cls;
    m.pose = PoseOf(r.loc, r.rot);
    m.flags = snapshot ? W::kFlagSnapshot : 0;
    switch (r.kind) {
        case Kind::Hole: m.op = W::StateOp::Hole; m.mat = r.mat; break;
        case Kind::Mound: m.op = W::StateOp::Mound; m.blob = r.blob; break;
        case Kind::Plant: m.op = W::StateOp::Plant; m.plant = r.plant; break;
    }
    return m;
}

const char* KindName(Kind k) { return k == Kind::Hole ? "hole" : (k == Kind::Mound ? "mound" : "plant"); }

// Name a live actor the host has not named; false when it is named, or the table is full, or it does not read.
bool Register(Kind kind, void* actor, bool announce) {
    if (!actor || g_hostByActor.count(actor)) return false;
    if (g_host.size() >= kMaxRows) {
        if (!g_rowsFullSaid) {
            g_rowsFullSaid = true;
            UE_LOGW("[DIG] host: %zu holes, mounds and plants are named already -- the rest stay on the host alone",
                    g_host.size());
        }
        return false;
    }
    HostRow r;
    r.kind = kind;
    r.cls = Narrow(R::ClassNameOf(actor));
    if (kind == Kind::Hole) {
        D::HoleState st;
        if (!D::ReadHole(actor, st)) return false;
        r.loc = st.location; r.rot = st.rotation; r.mat = st.mat;
    } else if (kind == Kind::Mound) {
        if (!D::ReadBurial(actor, r.loc, r.rot, r.blob) || r.blob.empty() || r.blob.size() > W::kMaxBlobBytes) return false;
    } else {
        std::wstring nm;
        if (!D::ReadPlantName(actor, nm) || !E::TryGetActorLocation(actor, r.loc) || !E::TryGetActorRotation(actor, r.rot))
            return false;
        r.plant = Narrow(nm);
    }
    r.id = g_nextId++;
    r.actor.Set(actor);
    g_hostByActor[actor] = r.id;
    ++g_registered;
    UE_LOGI("[DIG] host: %s #%u (%s) at (%.0f,%.0f,%.0f)%s%s", KindName(kind), r.id, r.cls.c_str(),
            static_cast<double>(r.loc.X), static_cast<double>(r.loc.Y), static_cast<double>(r.loc.Z),
            kind == Kind::Mound ? " holding a buried item" : "", kind == Kind::Plant ? (" plant=" + r.plant).c_str() : "");
    if (announce) Enqueue(-1, W::Pack(StateOf(r, false)));
    g_host.emplace(r.id, std::move(r));
    return true;
}

void Census(coop::net::Session& s) {
    // The dead first: a number is spent once, and a pointer the engine reuses must find no row.
    for (auto it = g_host.begin(); it != g_host.end();) {
        if (it->second.actor.Alive()) { ++it; continue; }
        W::State gone;
        gone.op = W::StateOp::Gone;
        gone.id = it->first;
        Enqueue(-1, W::Pack(gone));
        ++g_gone;
        UE_LOGI("[DIG] host: %s #%u is gone", KindName(it->second.kind), it->first);
        g_hostByActor.erase(it->second.actor.Raw());
        it = g_host.erase(it);
    }
    (void)s;
    for (void* h : D::AllHoles()) Register(Kind::Hole, h, true);
    for (void* m : D::AllMounds()) Register(Kind::Mound, m, true);   // a loot mound holds no class: ReadBurial refuses it
}

HostRow* NearestHole(const FVector& at, float maxUU) {
    HostRow* best = nullptr;
    float bestD = maxUU * maxUU;
    for (auto& [id, r] : g_host) {
        if (r.kind != Kind::Hole || !r.actor.Alive()) continue;
        const float d = Dist2(r.loc, at);
        if (d <= bestD) { bestD = d; best = &r; }
    }
    return best;
}

void SendAck(int slot, uint32_t reqId, bool ok, const char* why) {
    W::State a;
    a.op = W::StateOp::Ack;
    a.id = reqId;
    a.ok = ok ? 1 : 0;
    Enqueue(slot, W::Pack(a));
    if (!ok) { ++g_denied; UE_LOGW("[DIG] host: slot %d's request %u refused -- %s", slot, reqId, why); }
}

struct QueuedIntent {
    W::Intent m;
    uint64_t  atMs = 0;
};
struct Slot {
    coop::net::IntentQueue<QueuedIntent> queue;
};
Slot g_slots[coop::net::kMaxPeers];

// The class a client names, if it is one the host may spawn for that verb.
void* HoleClassFor(const std::string& name) {
    void* cls = ue_wrap::object_index::ClassByName(Widen(name).c_str());
    return (cls && D::IsHoleClass(cls)) ? cls : nullptr;
}

coop::net::Served Execute(coop::net::Session& s, const QueuedIntent& q, uint8_t slot) {
    using coop::net::Served;
    const W::Intent& m = q.m;
    if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) {
        SendAck(slot, m.reqId, false, "the sender's player is not alive");
        return Served::Done;
    }
    const EL::IntentTarget reach = EL::IntentTarget::ForClientIntent(s, slot, kReachUU);
    if (!reach.HasBody()) {
        if (NowMs() - q.atMs < kBodyWaitMs) return Served::Wait;
        SendAck(slot, m.reqId, false, "the host has no body for the sender");
        return Served::Done;
    }
    const FVector at = LocOf(m.pose);
    if (!reach.AuthorizeSegment(at, at)) {
        SendAck(slot, m.reqId, false, "the place is out of the sender's reach");
        return Served::Done;
    }
    switch (m.op) {
        case W::IntentOp::Dig: {
            void* cls = HoleClassFor(m.cls);
            if (!cls) { SendAck(slot, m.reqId, false, "not a hole class"); return Served::Done; }
            if (NearestHole(at, kSameHoleUU)) {
                SendAck(slot, m.reqId, true, "");   // a second press on the same hole: the first stands
                return Served::Done;
            }
            if (g_host.size() >= kMaxRows) { SendAck(slot, m.reqId, false, "too many holes"); return Served::Done; }
            D::HoleState st;
            st.location = at; st.rotation = RotOf(m.pose); st.mat = m.mat;
            void* hole = D::SpawnHole(cls, st);
            if (!hole) { SendAck(slot, m.reqId, false, "the hole did not spawn"); return Served::Done; }
            ++g_digsRun;
            UE_LOGI("[DIG] host: ran slot %u's dig (request %u): %s at (%.0f,%.0f,%.0f) mat %d", static_cast<unsigned>(slot),
                    m.reqId, m.cls.c_str(), static_cast<double>(at.X), static_cast<double>(at.Y), static_cast<double>(at.Z), m.mat);
            Register(Kind::Hole, hole, true);
            // The hole's own BeginPlay has dug up whatever mound lay under it; the census names that on its next turn.
            SendAck(slot, m.reqId, true, "");
            return Served::Done;
        }
        case W::IntentOp::Fill: {
            HostRow* row = NearestHole(at, kBuryNearUU);
            void* hole = row ? row->actor.Get() : nullptr;
            if (!hole) { SendAck(slot, m.reqId, false, "no such hole"); return Served::Done; }
            ++g_fillsRun;
            UE_LOGI("[DIG] host: ran slot %u's fill (request %u) of hole #%u", static_cast<unsigned>(slot), m.reqId, row->id);
            E::DestroyActor(hole);
            SendAck(slot, m.reqId, true, "");
            return Served::Done;
        }
        case W::IntentOp::Bury: {
            HostRow* row = NearestHole(at, kBuryNearUU);
            void* hole = row ? row->actor.Get() : nullptr;
            if (!hole) { SendAck(slot, m.reqId, false, "no such hole"); return Served::Done; }
            void* icls = ue_wrap::object_index::ClassByName(Widen(m.cls).c_str());
            if (!icls || !ue_wrap::prop::IsClassDescendantOfProp(icls) || D::IsHoleClass(icls) || D::IsMoundClass(icls)) {
                SendAck(slot, m.reqId, false, "the item's class is not a prop the host has");
                return Served::Done;
            }
            // The item the client held, rebuilt here from its own record, as a mirror: no peer is told of it.
            void* item = nullptr;
            {
                coop::prop_echo_suppress::ScopedMirrorSpawn guard;
                FVector up = row->loc;
                up.Z += 40.0f;
                item = D::SpawnItem(icls, up, m.blob);
                if (item) coop::prop_echo_suppress::MarkMirrorSpawn(item);
            }
            if (!item) { SendAck(slot, m.reqId, false, "the item did not rebuild"); return Served::Done; }
            if (D::FilterBlocks(hole, item)) {
                coop::prop_echo_suppress::MarkIncomingDestroy(item);
                E::DestroyActor(item);
                SendAck(slot, m.reqId, false, "the hole refuses this item");
                return Served::Done;
            }
            bool ok = false;
            g_capturedPlants.clear();
            const bool called = D::ProcessBuried(hole, item, ok);
            if (item && R::IsLive(item)) {   // the plush leaves its plush, a refused item stays: neither is buried
                coop::prop_echo_suppress::MarkIncomingDestroy(item);
                E::DestroyActor(item);
            }
            if (!called || !ok) { SendAck(slot, m.reqId, false, "processBuriedItem refused the item"); return Served::Done; }
            ++g_buriesRun;
            UE_LOGI("[DIG] host: ran slot %u's burial (request %u) of a %s in hole #%u", static_cast<unsigned>(slot), m.reqId,
                    m.cls.c_str(), row->id);
            // What the game's tail does after a good burial: the hole is spent. The mound or plant it made is named now.
            if (hole && R::IsLive(hole)) E::DestroyActor(hole);
            Census(s);
            SendAck(slot, m.reqId, true, "");
            return Served::Done;
        }
    }
    return Served::Done;
}

void HostTick(coop::net::Session& s) {
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot)
        g_slots[slot].queue.Serve(kBudget, NowMs, [&](const QueuedIntent& q) { return Execute(s, q, slot); });
    const uint64_t now = NowMs();
    if (now >= g_nextCensus) {
        g_nextCensus = now + kCensusMs;
        Census(s);
    }
}

// The gate: a burial's plants. The plant spawns and begins play inside processBuriedItem, a mound too, and only the
// plants have no census, so they are caught here.
sg::Verdict OnBuryPre(const sg::Call&) {
    if (!IsHost(g_session.load(std::memory_order_acquire))) return sg::Verdict::Run;
    ++g_buryDepth;
    return sg::Verdict::Run;
}

void OnBuryPost(const sg::Call&) {
    if (g_buryDepth > 0) --g_buryDepth;
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!IsHost(s) || g_buryDepth > 0) return;
    for (CachedObjRef& p : g_capturedPlants)
        if (void* a = p.Get()) Register(Kind::Plant, a, true);
    g_capturedPlants.clear();
}

sg::Verdict OnPlantBeginPlayPre(const sg::Call& c) {
    if (g_buryDepth > 0 && g_capturedPlants.size() < 8) {
        CachedObjRef r;
        r.Set(c.object);
        g_capturedPlants.push_back(r);
    }
    return sg::Verdict::Run;
}

// ---- CLIENT ----

struct ClientRow {
    uint32_t     id = 0;
    Kind         kind = Kind::Hole;
    CachedObjRef actor;
};
std::unordered_map<uint32_t, ClientRow> g_crows;

struct Parked {
    W::State m;
    uint64_t sinceMs = 0;
};
std::vector<Parked> g_parked;
uint64_t g_nextRetry = 0;

struct CapturedDig {
    CachedObjRef hole;
    std::string  cls;
    D::HoleState st;
};
std::vector<CapturedDig> g_digs;

struct PendingAck {
    CachedObjRef item;
    std::string  cls;
    uint64_t     untilMs = 0;
};
std::unordered_map<uint32_t, PendingAck> g_pending;
uint32_t g_nextReq = 1;
uint64_t g_appliedRows = 0, g_adopted = 0, g_spawned = 0, g_goneApplied = 0, g_digsSent = 0, g_burySent = 0,
         g_refusedBodies = 0;

std::unordered_set<void*> BoundSet() {
    std::unordered_set<void*> out;
    for (auto& [id, r] : g_crows)
        if (void* a = r.actor.Get()) out.insert(a);
    return out;
}

// A native actor of the row's family at the row's place that no row has bound.
void* AdoptNative(Kind kind, const W::State& m) {
    const std::unordered_set<void*> bound = BoundSet();
    const FVector at = LocOf(m.pose);
    const float lim = kAdoptUU * kAdoptUU;
    std::vector<void*> all = kind == Kind::Hole ? D::AllHoles() : (kind == Kind::Mound ? D::AllMounds() : D::AllPlants());
    void* best = nullptr;
    float bestD = lim;
    for (void* a : all) {
        if (bound.count(a)) continue;
        FVector l{};
        if (!E::TryGetActorLocation(a, l)) continue;
        if (Narrow(R::ClassNameOf(a)) != m.cls) continue;
        const float d = Dist2(l, at);
        if (d <= bestD) { bestD = d; best = a; }
    }
    return best;
}

void* SpawnMirror(Kind kind, const W::State& m) {
    void* cls = ue_wrap::object_index::ClassByName(Widen(m.cls).c_str());
    if (!cls) return nullptr;
    void* a = nullptr;
    ++g_mirrorSpawning;
    {
        coop::prop_echo_suppress::ScopedMirrorSpawn guard;
        if (kind == Kind::Hole) {
            D::HoleState st;
            st.location = LocOf(m.pose); st.rotation = RotOf(m.pose); st.mat = m.mat;
            a = D::SpawnHole(cls, st);
        } else if (kind == Kind::Mound) {
            a = D::SpawnMound(cls, LocOf(m.pose), RotOf(m.pose), m.blob);
        } else {
            a = D::SpawnPlant(cls, LocOf(m.pose), RotOf(m.pose), Widen(m.plant));
        }
        if (a) coop::prop_echo_suppress::MarkMirrorSpawn(a);
    }
    --g_mirrorSpawning;
    return a;
}

// True when the row is dealt with (bound, or refused for good); false when it should be tried again.
bool ApplyRow(const W::State& m, Kind kind) {
    auto it = g_crows.find(m.id);
    if (it != g_crows.end() && it->second.actor.Alive()) return true;   // a repeat
    if (it != g_crows.end()) g_crows.erase(it);
    void* a = AdoptNative(kind, m);
    const bool adopted = a != nullptr;
    if (!a) a = SpawnMirror(kind, m);
    if (!a) return false;
    ClientRow r;
    r.id = m.id;
    r.kind = kind;
    r.actor.Set(a);
    g_crows[m.id] = r;
    ++g_appliedRows;
    adopted ? ++g_adopted : ++g_spawned;
    UE_LOGI("[DIG] client: %s #%u (%s) %s at (%.0f,%.0f,%.0f)", KindName(kind), m.id, m.cls.c_str(),
            adopted ? "adopted from this peer's own world" : "spawned", static_cast<double>(m.pose.x),
            static_cast<double>(m.pose.y), static_cast<double>(m.pose.z));
    return true;
}

void ApplyState(const W::State& m) {
    switch (m.op) {
        case W::StateOp::Hole:
            if (!ApplyRow(m, Kind::Hole)) g_parked.push_back(Parked{m, NowMs()});
            return;
        case W::StateOp::Mound:
            if (!ApplyRow(m, Kind::Mound)) g_parked.push_back(Parked{m, NowMs()});
            return;
        case W::StateOp::Plant:
            if (!ApplyRow(m, Kind::Plant)) g_parked.push_back(Parked{m, NowMs()});
            return;
        case W::StateOp::Gone: {
            for (auto p = g_parked.begin(); p != g_parked.end();)
                p = (p->m.id == m.id && p->m.op != W::StateOp::Gone) ? g_parked.erase(p) : p + 1;
            auto it = g_crows.find(m.id);
            if (it == g_crows.end()) return;
            if (void* a = it->second.actor.Get()) {
                coop::prop_echo_suppress::MarkIncomingDestroy(a);
                E::DestroyActor(a);
            }
            ++g_goneApplied;
            UE_LOGI("[DIG] client: %s #%u is gone on the host's word", KindName(it->second.kind), m.id);
            g_crows.erase(it);
            return;
        }
        case W::StateOp::SnapshotEnd:
            UE_LOGI("[DIG] client: the host's snapshot ended -- %zu rows bound (%llu adopted, %llu spawned), %zu parked",
                    g_crows.size(), static_cast<unsigned long long>(g_adopted), static_cast<unsigned long long>(g_spawned),
                    g_parked.size());
            return;
        case W::StateOp::Ack: {
            auto it = g_pending.find(m.id);
            if (it == g_pending.end()) return;
            PendingAck p = std::move(it->second);
            g_pending.erase(it);
            if (!m.ok) {
                UE_LOGW("[DIG] client: the host refused request %u -- the %s stays in hand", m.id, p.cls.c_str());
                return;
            }
            // The host did it: the item the burial or fill spent leaves this hand, as the game's own tail would.
            void* item = p.item.Get();
            if (item && Narrow(R::ClassNameOf(item)) == p.cls) E::DestroyActor(item);
            UE_LOGI("[DIG] client: the host ran request %u -- the %s is spent", m.id, p.cls.c_str());
            return;
        }
    }
}

void ClientTick(coop::net::Session& s) {
    (void)s;
    const uint64_t now = NowMs();
    // A dig the shovel made here: the intent goes, the local hole does not stay (the host's is the one every peer has).
    if (!g_digs.empty()) {
        std::vector<CapturedDig> digs;
        digs.swap(g_digs);
        for (CapturedDig& d : digs) {
            W::Intent i;
            i.op = W::IntentOp::Dig;
            i.reqId = g_nextReq++;
            i.cls = d.cls;
            i.pose = PoseOf(d.st.location, d.st.rotation);
            i.mat = d.st.mat;
            Enqueue(-1, W::Pack(i));
            ++g_digsSent;
            UE_LOGI("[DIG] client: dig #%u sent: %s at (%.0f,%.0f,%.0f) mat %d", i.reqId, i.cls.c_str(),
                    static_cast<double>(i.pose.x), static_cast<double>(i.pose.y), static_cast<double>(i.pose.z), i.mat);
            if (void* h = d.hole.Get()) {
                coop::prop_echo_suppress::MarkIncomingDestroy(h);
                E::DestroyActor(h);
            }
        }
    }
    for (auto it = g_pending.begin(); it != g_pending.end();)
        it = (now > it->second.untilMs) ? g_pending.erase(it) : std::next(it);
    if (now >= g_nextRetry && !g_parked.empty()) {
        g_nextRetry = now + 500;
        std::vector<Parked> keep;
        for (Parked& p : g_parked) {
            const Kind k = p.m.op == W::StateOp::Hole ? Kind::Hole : (p.m.op == W::StateOp::Mound ? Kind::Mound : Kind::Plant);
            if (now - p.sinceMs > kParkMs) {
                UE_LOGW("[DIG] client: %s #%u could not be made in %llu s -- left out", KindName(k), p.m.id,
                        static_cast<unsigned long long>(kParkMs / 1000));
                continue;
            }
            if (!ApplyRow(p.m, k)) keep.push_back(std::move(p));
        }
        g_parked.swap(keep);
    }
}

// A mirror hole's BeginPlay digs a mound up and attaches the hole to what it was dug into: both are the host's. A
// hole the local shovel just made is read here (the hole is built, the material written) and sent as the dig.
sg::Verdict OnHoleBeginPlayPre(const sg::Call& c) {
    if (!IsClient(g_session.load(std::memory_order_acquire))) return sg::Verdict::Run;
    ++g_refusedBodies;
    if (g_mirrorSpawning > 0 || g_digDepth == 0) return sg::Verdict::Cancel;
    D::HoleState st;
    if (g_digs.size() < kMaxPendingIntents && D::ReadHole(c.object, st)) {
        CapturedDig d;
        d.hole.Set(c.object);
        d.cls = Narrow(R::ClassNameOf(c.object));
        d.st = st;
        g_digs.push_back(std::move(d));
    }
    return sg::Verdict::Cancel;
}

sg::Verdict OnHoleOverlapPre(const sg::Call&) {
    return IsClient(g_session.load(std::memory_order_acquire)) ? sg::Verdict::Cancel : sg::Verdict::Run;
}

sg::Verdict OnDigPre(const sg::Call&) {
    ++g_digDepth;
    return sg::Verdict::Run;
}
void OnDigPost(const sg::Call&) {
    if (g_digDepth > 0) --g_digDepth;
}

ue_wrap::reflection::ParamOffset g_holdParam{L"holdObject"};

// A held item used on a hole: a buried item or a planted seed (its record goes with the intent), or a dirt ball (the
// hole is filled). The client's own body would spend the item and the hole here with no word to the host.
sg::Verdict OnHoleUsePre(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!IsClient(s) || !D::IsHole(c.object)) return sg::Verdict::Run;
    const int32_t off = g_holdParam.Of(c.function);
    void* held = nullptr;
    if (off >= 0 && c.locals) std::memcpy(&held, c.locals + off, sizeof(held));
    if (!held || !R::IsLive(held)) return sg::Verdict::Run;   // nothing held: the body does nothing either
    if (g_pending.size() >= kMaxPendingIntents) return sg::Verdict::Cancel;
    D::HoleState hs;
    if (!D::ReadHole(c.object, hs)) return sg::Verdict::Run;
    W::Intent i;
    i.reqId = g_nextReq;
    i.pose = PoseOf(hs.location, hs.rotation);
    PendingAck p;
    p.item.Set(held);
    p.cls = Narrow(R::ClassNameOf(held));
    p.untilMs = NowMs() + kAckWaitMs;
    if (D::IsDirtBall(held)) {
        i.op = W::IntentOp::Fill;
    } else {
        // The body's own refusal ("Cannot bury this item") stays the game's: it spends nothing.
        if (D::FilterBlocks(c.object, held)) return sg::Verdict::Run;
        i.op = W::IntentOp::Bury;
        i.cls = p.cls;
        if (!D::ReadItemRecord(held, i.blob)) i.blob.clear();   // an item with no record is rebuilt as it is born
        if (i.blob.size() > W::kMaxBlobBytes) i.blob.clear();
    }
    ++g_nextReq;
    g_pending[i.reqId] = std::move(p);
    Enqueue(-1, W::Pack(i));
    ++g_burySent;
    UE_LOGI("[DIG] client: %s #%u sent: a %s on the hole at (%.0f,%.0f,%.0f), record %zu B",
            i.op == W::IntentOp::Fill ? "fill" : "burial", i.reqId, g_pending[i.reqId].cls.c_str(),
            static_cast<double>(i.pose.x), static_cast<double>(i.pose.y), static_cast<double>(i.pose.z), i.blob.size());
    return sg::Verdict::Cancel;
}

// ---- the watches ----

const Watch kWatches[] = {
    {&kRowHoleBeginPlay, kTagHoleBeginPlay, "dirthole_C.ReceiveBeginPlay", &OnHoleBeginPlayPre, nullptr},
    {&kRowHoleUse, kTagHoleUse, "dirthole_C.playerUsedOn", &OnHoleUsePre, nullptr},
    {&kRowHoleOverlap, kTagHoleOverlap, "dirthole_C box overlap", &OnHoleOverlapPre, nullptr},
    {&kRowShovelDig, kTagShovelDig, "prop_shovel_C.event_dighole", &OnDigPre, &OnDigPost},
    {&kRowBury, kTagBury, "dirthole_C.processBuriedItem", &OnBuryPre, &OnBuryPost},
    {&kRowPlantBeginPlay, kTagPlantBeginPlay, "growingPlant_C.ReceiveBeginPlay", &OnPlantBeginPlayPre, nullptr},
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kWatchCount] = {};
bool g_settled = false;

void RegisterWatches() {
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        const Watch& w = kWatches[i];
        if (sg::WatchName(*w.row, w.tag, w.pre, w.post)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("[DIG] the script-body gate refused the watch on %s -- that dig verb stays on its own peer", w.what);
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
        UE_LOGI("[DIG] the dig watches are live: hole begin-play, use, fill overlap, dig, burial and plant begin-play");
    } else if (pending == 0 && sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[DIG] %d of %d dig watches are dead -- those verbs stay on their own peer", kWatchCount - live, kWatchCount);
    }
}

coop::blob_chunks::Assembler g_asmIntent, g_asmState;

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!session) return;
    RegisterWatches();
}

void Tick(coop::net::Session& session) {
    if (!g_settled) {
        sg::ResolvePendingNames();
        Settle();
    }
    if (!session.running()) return;
    if (session.role() == coop::net::Role::Host) HostTick(session);
    else ClientTick(session);
    DrainOut(session);
    if (!g_asmIntent.Idle() || !g_asmState.Idle()) {
        static uint64_t s_nextSweep = 0;
        if (NowMs() >= s_nextSweep) {
            s_nextSweep = NowMs() + 1000;
            g_asmIntent.Sweep(std::chrono::steady_clock::now(), std::chrono::seconds(10));
            g_asmState.Sweep(std::chrono::steady_clock::now(), std::chrono::seconds(10));
        }
    }
}

void OnPeerWorldReady(int slot) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!IsHost(s) || slot < 1 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    Census(*s);
    int sent = 0;
    for (const auto& [id, r] : g_host) {
        if (!r.actor.Alive()) continue;
        Enqueue(slot, W::Pack(StateOf(r, true)));
        ++sent;
    }
    W::State end;
    end.op = W::StateOp::SnapshotEnd;
    Enqueue(slot, W::Pack(end));
    UE_LOGI("[DIG] host: slot %d's world is up -- %d row(s) and the snapshot's end queued", slot, sent);
}

void OnIntentChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!IsHost(s) || senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    std::vector<uint8_t> blob;
    if (!g_asmIntent.OnChunk(p, senderSlot, blob)) return;
    W::Intent m;
    if (!W::Parse(blob, m)) {
        ++g_dropped;
        UE_LOGW("[DIG] host: a malformed DigIntent (%zu B) from slot %u dropped", blob.size(), static_cast<unsigned>(senderSlot));
        return;
    }
    QueuedIntent q;
    q.m = std::move(m);
    q.atMs = NowMs();
    const uint32_t req = q.m.reqId;
    if (!g_slots[senderSlot].queue.Push(std::move(q), kMaxPendingIntents)) {
        SendAck(senderSlot, req, false, "queue full");
        return;
    }
    UE_LOGI("[DIG] host: received slot %u's intent %u (op %d)", static_cast<unsigned>(senderSlot), req, static_cast<int>(m.op));
}

void OnStateChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!IsClient(s) || senderSlot != 0) return;
    std::vector<uint8_t> blob;
    if (!g_asmState.OnChunk(p, senderSlot, blob)) return;
    W::State m;
    if (!W::Parse(blob, m)) {
        ++g_dropped;
        UE_LOGW("[DIG] client: a malformed DigState (%zu B) dropped", blob.size());
        return;
    }
    ApplyState(m);
}

void OnDisconnect() {
    UE_LOGI("[DIG] session ended: host named %llu (gone %llu), ran %llu digs %llu burials %llu fills, refused %llu; client "
            "bound %zu (adopted %llu spawned %llu), sent %llu digs %llu burials, refused %llu bodies",
            static_cast<unsigned long long>(g_registered), static_cast<unsigned long long>(g_gone),
            static_cast<unsigned long long>(g_digsRun), static_cast<unsigned long long>(g_buriesRun),
            static_cast<unsigned long long>(g_fillsRun), static_cast<unsigned long long>(g_denied), g_crows.size(),
            static_cast<unsigned long long>(g_adopted), static_cast<unsigned long long>(g_spawned),
            static_cast<unsigned long long>(g_digsSent), static_cast<unsigned long long>(g_burySent),
            static_cast<unsigned long long>(g_refusedBodies));
    g_host.clear();
    g_hostByActor.clear();
    g_crows.clear();
    g_parked.clear();
    g_digs.clear();
    g_pending.clear();
    g_out.clear();
    g_capturedPlants.clear();
    for (auto& sl : g_slots) sl = Slot{};
    g_asmIntent.Clear();
    g_asmState.Clear();
    g_nextId = 1;
    g_nextReq = 1;
    g_nextCensus = 0;
    g_digDepth = g_buryDepth = g_mirrorSpawning = 0;
    g_rowsFullSaid = g_fullSaid = false;
    g_registered = g_gone = g_digsRun = g_buriesRun = g_fillsRun = g_denied = g_dropped = 0;
    g_appliedRows = g_adopted = g_spawned = g_goneApplied = g_digsSent = g_burySent = g_refusedBodies = 0;
}

}  // namespace coop::dig_sync
