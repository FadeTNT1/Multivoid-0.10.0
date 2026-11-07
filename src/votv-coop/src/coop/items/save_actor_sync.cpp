// coop/items/save_actor_sync.cpp -- see coop/items/save_actor_sync.h.

#include "coop/items/save_actor_sync.h"

#include "coop/element/intent_authority.h"
#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_spawn_authoring.h"
#include "coop/session/world_load_episode.h"

#include "ue_wrap/actors/save_actor.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"  // ClassByName: the host's O(1) class resolve for a client's record
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/actor_end_play.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/keyed_objects.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace coop::save_actor_sync {
namespace {

namespace E  = ue_wrap::engine;
namespace EP = ue_wrap::actor_end_play;
namespace GT = ue_wrap::game_thread;
namespace KO = ue_wrap::keyed_objects;
namespace N  = ue_wrap::profile::name;
namespace R  = ue_wrap::reflection;
namespace SA = ue_wrap::save_actor;
namespace SR = ue_wrap::save_record;
namespace W  = coop::save_record_wire;
namespace sg = ue_wrap::script_gate;
using ue_wrap::CachedObjRef;
using ue_wrap::FVector;
using SaveRecord = SR::SaveRecord;

// The birth seam, and the mutators whose run changes an actor's record (each found in the 0.10.0 capture: the light
// string's addLight on every click after the first and finish on rmb, the weld's completion, a candle lit by a flame or
// a lighter, a rug's picked texture, a jumper cable's plug).
UE_FACT_FN(kRowFinishSpawn, "save_actor_sync", L"GameplayStatics", L"FinishSpawningActor", Declared);
UE_FACT_FN(kRowXmasAddLight, "save_actor_sync", L"xmaslight_C", L"addLight", Declared);
UE_FACT_FN(kRowXmasFinish, "save_actor_sync", L"xmaslight_C", L"finish", Declared);
UE_FACT_FN(kRowWeld, "save_actor_sync", L"weld_C", L"weld", Declared);
UE_FACT_FN(kRowCandleIgnite, "save_actor_sync", L"candle_C", L"ignite", Declared);
UE_FACT_FN(kRowCandleAttempt, "save_actor_sync", L"candle_C", L"attemptIgnite", Declared);
UE_FACT_FN(kRowRugSkin, "save_actor_sync", L"rug_C", L"setSkin", Declared);
UE_FACT_FN(kRowCableConnect, "save_actor_sync", L"jumpercable_C", L"connect", Declared);

struct Watch {
    const ue_wrap::facts::Function* row;
    int                             tag;
};
constexpr int kTagBase = 0x53415331;   // 'SAS1'
const Watch kWatches[] = {
    {&kRowXmasAddLight, kTagBase + 0}, {&kRowXmasFinish, kTagBase + 1},   {&kRowWeld, kTagBase + 2},
    {&kRowCandleIgnite, kTagBase + 3}, {&kRowCandleAttempt, kTagBase + 4}, {&kRowRugSkin, kTagBase + 5},
    {&kRowCableConnect, kTagBase + 6},
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));

constexpr float    kBirthReachUU = 4000.f;   // a nail flies; a sticker or a candle is placed at arm's length
constexpr float    kVerbReachUU = 1500.f;    // a pickup, a light, a click on what is there
constexpr size_t   kMaxCaught = 128;
constexpr size_t   kMaxDeaths = 256;
constexpr size_t   kMaxDirty = 64;
constexpr size_t   kMaxRows = 2048;
constexpr size_t   kMaxTombs = 256;
constexpr size_t   kMaxQueuedOut = 512;
constexpr size_t   kMaxParked = 64;
constexpr size_t   kMaxIncoming = 512;
constexpr size_t   kMaxReqs = 64;
constexpr uint64_t kReqTimeoutMs = 15000;
constexpr uint64_t kParkMs = 30000;
constexpr uint64_t kEchoQuietMs = 1500;      // a client's own state, sent this recently, outranks the host's echo
constexpr uint32_t kBudgetPerWindow = 24;    // a client's intents per window
constexpr uint64_t kBudgetWindowMs = 2000;
constexpr uint8_t  kMaxCaptureTries = 6;

// Per allowlist index: what the class needs beyond the common path.
enum Policy : uint8_t {
    kHostConstraint = 1u << 0,   // processKeys ties two props: never on a mirror, and a client's own copy is replaced
    kSelfRetiring   = 1u << 1,   // destroys itself on every peer: a client's own death is never a verb to forward
};
std::vector<uint8_t> g_policy;

std::atomic<coop::net::Session*> g_session{nullptr};
bool     g_live = false;            // the seams act: a host running, or a client connected (set by Tick)
bool     g_host = false;
uint64_t g_tick = 0;
uint64_t g_nextRefreshMs = 0;
int      g_spawning = 0;            // our own spawn and apply: its births and mutators are ours
int      g_applying = 0;            // our own record apply: its mutators are ours
uint32_t g_keyNonce = 0;
uint32_t g_keySeq = 0;

bool g_seamInstalled = false, g_seamRefused = false;
bool g_sinkInstalled = false, g_sinkRefused = false;
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kWatchCount] = {};
bool g_settled = false;

uint64_t NowMs() { return coop::net::NowMs(); }

coop::net::Session* Session() { return g_session.load(std::memory_order_acquire); }

// ---- the wire grammar (one blob per message) --------------------------------------------------------------------
//
//   op 1 Birth   : u8 owner | u16 req | u8 flags | u32 betterLen | better | SaveRecord
//   op 2 State   : u32 betterLen | better | SaveRecord
//   op 3 Destroy : wstr key
//   op 4 Refused : u16 req
// `better` is the actor's struct_betterSave as ue_wrap/actors/inventory_item_codec encodes it, empty when this peer
// could not capture one; SaveRecord is the legacy getData record, which always rides: its class, transform and key are
// the message's identity.
enum Op : uint8_t { kBirth = 1, kState = 2, kDestroy = 3, kRefused = 4 };
enum Flag : uint8_t { kFlagSnapshot = 1u << 0 };

struct Msg {
    uint8_t              op = 0;
    uint8_t              owner = 0;
    uint16_t             req = 0;
    uint8_t              flags = 0;
    std::vector<uint8_t> better;
    SaveRecord           rec;
    std::wstring         key;   // Destroy
};

void PackBody(std::vector<uint8_t>& b, const std::vector<uint8_t>& better, const SaveRecord& rec) {
    W::AppU32(b, static_cast<uint32_t>(better.size()));
    b.insert(b.end(), better.begin(), better.end());
    W::SerSave(b, rec);
}

std::vector<uint8_t> Pack(const Msg& m) {
    std::vector<uint8_t> b;
    W::AppU8(b, m.op);
    switch (m.op) {
        case kBirth:
            W::AppU8(b, m.owner);
            W::AppU16(b, m.req);
            W::AppU8(b, m.flags);
            PackBody(b, m.better, m.rec);
            break;
        case kState: PackBody(b, m.better, m.rec); break;
        case kDestroy: W::AppWStr(b, m.key); break;
        case kRefused: W::AppU16(b, m.req); break;
        default: break;
    }
    // The 0.10.0 record is the larger half; a message past the transport goes with the legacy record alone.
    if (b.size() > coop::blob_chunks::MaxBlobBytes() && !m.better.empty()) {
        Msg lean = m;
        lean.better.clear();
        return Pack(lean);
    }
    return b;
}

bool ParseBody(const std::vector<uint8_t>& b, size_t& o, Msg& m) {
    uint32_t n = 0;
    if (!W::RdU32(b, o, n) || n > b.size() - o) return false;
    m.better.assign(b.begin() + static_cast<std::ptrdiff_t>(o), b.begin() + static_cast<std::ptrdiff_t>(o + n));
    o += n;
    return W::DeSave(b, o, m.rec);
}

bool Parse(const std::vector<uint8_t>& b, Msg& m) {
    size_t o = 0;
    if (!W::RdU8(b, o, m.op)) return false;
    switch (m.op) {
        case kBirth:
            if (!W::RdU8(b, o, m.owner) || !W::RdU16(b, o, m.req) || !W::RdU8(b, o, m.flags)) return false;
            if (!ParseBody(b, o, m)) return false;
            break;
        case kState:
            if (!ParseBody(b, o, m)) return false;
            break;
        case kDestroy:
            if (!W::RdWStr(b, o, m.key)) return false;
            break;
        case kRefused:
            if (!W::RdU16(b, o, m.req)) return false;
            break;
        default: return false;
    }
    return o == b.size();
}

// The record's identity in one number: what a mutator's republish compares against, so a run that changed nothing
// sends nothing.
uint64_t HashOf(const std::vector<uint8_t>& better, const SaveRecord& rec) {
    std::vector<uint8_t> b;
    PackBody(b, better, rec);
    return coop::blob_chunks::Fnv64(b);
}

bool InWorld(float v) { return std::isfinite(v) && std::fabs(v) <= coop::net::kMaxCoord; }

// Every number a record carries, bounded: a record with an out-of-world number in it is refused whole, never clamped.
bool RecordSane(const SaveRecord& r) {
    for (size_t i = 0; i < r.xform.size(); ++i) {
        if (!std::isfinite(r.xform[i])) return false;
        if (i >= 4 && i <= 6 && !InWorld(r.xform[i])) return false;
    }
    for (const auto& g : r.vectors)
        for (const auto& v : g)
            if (!InWorld(v[0]) || !InWorld(v[1]) || !InWorld(v[2])) return false;
    for (const auto& g : r.rotators)
        for (const auto& v : g)
            if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) return false;
    for (const auto& g : r.floats)
        for (float f : g)
            if (!std::isfinite(f)) return false;
    for (const auto& g : r.transforms)
        for (const auto& t : g)
            for (float f : t)
                if (!std::isfinite(f)) return false;
    return true;
}

FVector LocOf(const SaveRecord& r) { return FVector{r.xform[4], r.xform[5], r.xform[6]}; }

// ---- tables ------------------------------------------------------------------------------------------------------

// A deployed actor this peer knows by key: the host's own and those it spawned for a client; a client's mirrors and
// the copies of its own the host's echo bound.
struct Row {
    CachedObjRef ref;
    int          cls = -1;
    uint8_t      owner = 0;
    bool         born = false;       // HOST: born this session (a joiner's save may predate it)
    uint64_t     hash = 0;           // the record last sent or applied
    uint64_t     localSendMs = 0;    // CLIENT: a state of its own sent then
};
std::unordered_map<std::wstring, Row> g_rows;
std::deque<std::wstring> g_tombs;   // HOST: the keys destroyed this session, for a joiner

// A birth the seam caught, worked a tick later (the Blueprint fills the actor's state after the finish).
struct Caught {
    CachedObjRef ref;
    int          cls = -1;
    uint64_t     tick = 0;
    uint8_t      tries = 0;
    bool         shadow = false;   // CLIENT: a birth an allowlisted actor's own step made; the host's to make
};
std::vector<Caught> g_caught;

struct Death {
    std::wstring key;
    int          cls = -1;
};
std::vector<Death> g_deaths;
std::unordered_set<void*> g_incoming;   // actors this lane destroys itself: their end of play is not news

std::vector<CachedObjRef> g_dirty;

// CLIENT: a birth asked of the host, by request id.
struct Req {
    uint16_t     id = 0;
    CachedObjRef ref;
    std::wstring key;
    int          cls = -1;
    bool         replaced = false;   // the copy went once its record was sent; the host's mirror takes its place
    uint64_t     sentMs = 0;
};
std::vector<Req> g_reqs;
uint16_t g_nextReq = 1;

// CLIENT: a birth whose class has not loaded here, or whose save copy the key table cannot be asked about yet.
struct Parked {
    Msg      m;
    uint64_t untilMs = 0;
};
std::vector<Parked> g_parked;

struct Budget {
    uint64_t windowStart = 0;
    uint32_t n = 0;
};
Budget g_budget[coop::net::kMaxPeers] = {};

// ---- counters (the summary line) ----
uint64_t g_cBorn = 0, g_cSpawnedForClient = 0, g_cMirrors = 0, g_cAdopted = 0, g_cBound = 0, g_cStates = 0,
         g_cDestroys = 0, g_cRefused = 0, g_cShadowed = 0, g_cLocalOnly = 0, g_cReplaced = 0, g_cDropped = 0;

// ---- the outbound queue: the host's state, a client's intents; a send the link refuses stays ----

struct Out {
    int                  slot;   // HOST: -1 every client, else that slot; CLIENT: the host
    std::vector<uint8_t> bytes;
};
std::deque<Out> g_out;
uint32_t g_sendSeq = 1;
bool g_outFullSaid = false;

void Enqueue(int slot, std::vector<uint8_t>&& bytes) {
    if (bytes.size() > coop::blob_chunks::MaxBlobBytes()) {
        UE_LOGW("[SAVE-ACTOR] a %zu-byte message is past the transport -- dropped", bytes.size());
        ++g_cDropped;
        return;
    }
    if (g_out.size() >= kMaxQueuedOut) {
        if (!g_outFullSaid) {
            g_outFullSaid = true;
            UE_LOGW("[SAVE-ACTOR] the outbound queue is full (%zu) -- a message is dropped", g_out.size());
        }
        ++g_cDropped;
        return;
    }
    g_out.push_back(Out{slot, std::move(bytes)});
}

// HOST: to every client, when one is there to hear it (the rows still record what a joiner is owed).
void Broadcast(const Msg& m) {
    auto* s = Session();
    if (!s || !s->connected()) return;
    Enqueue(-1, Pack(m));
}

void DrainOut(coop::net::Session& s) {
    if (!s.running() || !s.connected()) return;
    const bool host = s.role() == coop::net::Role::Host;
    const auto kind = host ? coop::net::ReliableKind::SaveActorState : coop::net::ReliableKind::SaveActorIntent;
    for (int n = 0; n < 16 && !g_out.empty(); ++n) {
        const Out& o = g_out.front();
        const bool ok = (host && o.slot >= 0) ? coop::blob_chunks::SendBlobToSlot(&s, o.slot, kind, g_sendSeq, o.bytes)
                                              : coop::blob_chunks::SendBlob(&s, kind, g_sendSeq, o.bytes);
        if (!ok) return;   // all or nothing: the whole blob again next tick, under a fresh number
        ++g_sendSeq;
        g_out.pop_front();
    }
}

// ---- helpers -----------------------------------------------------------------------------------------------------

// The allowlist name of an index, "" past it.
const wchar_t* ClsName(int ci) { return ci >= 0 ? SA::ClassNameAt(static_cast<size_t>(ci)) : L""; }

bool HasPolicy(int cls, uint8_t p) {
    return cls >= 0 && static_cast<size_t>(cls) < g_policy.size() && (g_policy[static_cast<size_t>(cls)] & p) != 0;
}

void ComputePolicies() {
    if (!g_policy.empty()) return;
    g_policy.assign(SA::Count(), 0);
    for (size_t i = 0; i < SA::Count(); ++i) {
        const wchar_t* name = SA::ClassNameAt(i);
        for (const wchar_t* c : N::kSaveActorHostConstraintClasses)
            if (::_wcsicmp(c, name) == 0) g_policy[i] |= kHostConstraint;
        for (const wchar_t* c : N::kSaveActorSelfRetiringClasses)
            if (::_wcsicmp(c, name) == 0) g_policy[i] |= kSelfRetiring;
    }
}

void MarkIncoming(void* actor) {
    if (!actor) return;
    if (g_incoming.size() >= kMaxIncoming) g_incoming.clear();   // an actor that never ended play: a bounded forget
    g_incoming.insert(actor);
}

void DestroyOurs(void* actor) {
    if (!actor) return;
    MarkIncoming(actor);
    E::DestroyActor(actor);
    // The end of play runs inside the destroy, and has taken the mark by now. One still here never ended play (a
    // refused destroy, an actor that never began play): forget it, so a later actor at its address is not muted.
    g_incoming.erase(actor);
}

std::wstring MintKey(uint8_t slot) {
    wchar_t buf[48];
    std::swprintf(buf, 48, L"mvsa_%08x_%u_%u", g_keyNonce, static_cast<unsigned>(slot), ++g_keySeq);
    return std::wstring(buf);
}

// The live actor `key` names: the row's, else the game's own key table (a save-loaded copy), of an allowlisted class.
void* FindByKey(const std::wstring& key, int* clsOut = nullptr) {
    void* a = nullptr;
    auto it = g_rows.find(key);
    if (it != g_rows.end()) a = it->second.ref.Get();
    if (!a && KO::Available()) a = KO::Resolve(key.c_str());
    if (!a) return nullptr;
    const int ci = SA::IndexOf(a);
    if (ci < 0) return nullptr;
    if (clsOut) *clsOut = ci;
    return a;
}

Row& PutRow(const std::wstring& key, void* actor, int cls, uint8_t owner, bool born, uint64_t hash) {
    if (g_rows.size() >= kMaxRows && g_rows.find(key) == g_rows.end()) {
        // The table is a cache of what the seams saw; a row past the bound is forgotten oldest-first in no particular
        // order -- the actor stays, found again through the key table.
        for (auto it = g_rows.begin(); it != g_rows.end();) {
            if (!it->second.ref.Alive()) it = g_rows.erase(it);
            else ++it;
        }
        if (g_rows.size() >= kMaxRows) g_rows.erase(g_rows.begin());
    }
    Row& r = g_rows[key];
    r.ref.Set(actor);
    r.cls = cls;
    r.owner = owner;
    r.born = r.born || born;
    r.hash = hash;
    return r;
}

// Capture both records of `actor` under its key. False when neither the legacy record nor its class resolve.
bool CaptureBoth(void* actor, int cls, const std::wstring& key, Msg& m) {
    m.better.clear();
    m.rec = SaveRecord{};
    if (!SA::CaptureRecord(actor, m.rec)) return false;
    SA::CaptureBetter(actor, m.better);   // optional: the codec measures off a class that may not be loaded
    m.rec.className = ClsName(cls);
    m.rec.key = key;
    return true;
}

// Apply a received record to `actor` under the canonical `key`: the 0.10.0 record when it came and decodes here, else
// the legacy one, then the key re-asserted and filed, then the actor's processKeys unless the class's ties are the
// host's alone and this is not the host.
void ApplyTo(void* actor, int cls, const std::wstring& key, const Msg& m, bool host) {
    SaveRecord rec = m.rec;
    rec.className = ClsName(cls);
    rec.key = key;
    ++g_applying;
    bool applied = !m.better.empty() && SA::ApplyBetter(actor, m.better);
    if (!applied) applied = SA::ApplyRecord(actor, rec, host ? &ue_wrap::object_index::ClassByName : nullptr);
    if (!applied)
        UE_LOGW("[SAVE-ACTOR] neither record applied to %ls '%ls' -- it stands as spawned", ClsName(cls),
                key.c_str());
    SA::WriteKey(actor, key);   // betterLoadData writes the record's key: the canonical one wins
    SA::EnsureKey(actor);       // filed in the gamemode's key table, so processKeys anywhere resolves it
    if (host || !HasPolicy(cls, kHostConstraint)) SA::ProcessKeys(actor);
    --g_applying;
}

bool SpendBudget(uint8_t slot) {
    if (slot >= coop::net::kMaxPeers) return false;
    Budget& b = g_budget[slot];
    const uint64_t now = NowMs();
    if (now - b.windowStart >= kBudgetWindowMs) {
        b.windowStart = now;
        b.n = 0;
    }
    if (b.n >= kBudgetPerWindow) return false;
    ++b.n;
    return true;
}

// ---- the seams ---------------------------------------------------------------------------------------------------

// Every finished deferred spawn in the game; only an allowlisted class goes further.
void OnFinishPost(void* /*context*/, void* source, void* spawned) {
    if (!g_live || !spawned || g_spawning > 0 || !GT::IsGameThread()) return;
    const int ci = SA::IndexOf(spawned);
    if (ci < 0) return;
    if (source && SA::IsGamemode(source)) return;   // a save load's: the save carries it
    if (g_caught.size() >= kMaxCaught) {
        ++g_cDropped;
        return;
    }
    Caught c;
    c.ref.Set(spawned);
    c.cls = ci;
    c.tick = g_tick;
    if (!g_host) {
        // A step an allowlisted actor takes itself (the scaffold becoming the wall) happens on the host's copy too;
        // the host's result is the one every peer keeps.
        if (source && SA::IndexOf(source) >= 0) {
            c.shadow = true;
        } else if (!coop::prop_spawn_authoring::BirthIsPlayerAuthored() &&
                   !coop::prop_spawn_authoring::IsAuthoredActor(source)) {
            ++g_cLocalOnly;
            return;
        }
    }
    g_caught.push_back(std::move(c));
}

void OnEndPlay(void* actor, EP::Reason reason) {
    if (reason != EP::Reason::Destroyed || !g_live) return;
    const int ci = SA::IndexOf(actor);
    if (ci < 0) return;
    if (g_incoming.erase(actor) > 0) return;
    std::wstring key = SA::KeyOf(actor);
    if (key.empty()) return;
    if (g_deaths.size() >= kMaxDeaths) {
        ++g_cDropped;
        return;
    }
    g_deaths.push_back(Death{std::move(key), ci});
}

void OnMutatorPost(const sg::Call& call) {
    if (!g_live || g_applying > 0 || g_spawning > 0 || !call.object) return;
    for (const CachedObjRef& d : g_dirty)
        if (d.Is(call.object)) return;
    if (g_dirty.size() >= kMaxDirty) return;
    CachedObjRef r;
    r.Set(call.object);
    g_dirty.push_back(r);
}

void InstallSeam() {
    if (g_seamInstalled || g_seamRefused) return;
    if (!R::FindClass(N::GameplayStaticsClass)) return;   // an engine class: present from boot, asked again until then
    const ue_wrap::facts::Resolved finish = ue_wrap::facts::Resolve(kRowFinishSpawn);
    if (!finish || !ue_wrap::ufunction_hook::InstallPostHook(finish, &OnFinishPost)) {
        g_seamRefused = true;
        UE_LOGE("[SAVE-ACTOR] the %ls seam did NOT install (%s) -- a sticker, candle, weld, nail or wall a player "
                "places stays on that peer alone this process",
                N::FinishSpawningActorFn, finish ? "native hook table full" : "the function is not on the class");
        return;
    }
    g_seamInstalled = true;
    UE_LOGI("[SAVE-ACTOR] installed on %ls -- %zu deployed actor_save classes are the host's", N::FinishSpawningActorFn,
            SA::Count());
}

void InstallSink() {
    if (g_sinkInstalled || g_sinkRefused) return;
    if (!EP::AddSink(&OnEndPlay)) {
        g_sinkRefused = true;
        UE_LOGE("[SAVE-ACTOR] the end-of-play sink table is full -- a deployed actor's removal stays on its own peer");
        return;
    }
    g_sinkInstalled = true;
}

void RegisterWatches() {
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        if (sg::WatchName(*kWatches[i].row, kWatches[i].tag, nullptr, &OnMutatorPost)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("[SAVE-ACTOR] the script-body gate refused a mutator watch (row %d) -- that change stays on its peer "
                "until the next birth or join", i);
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
        UE_LOGI("[SAVE-ACTOR] the mutator watches are live: light string addLight and finish, weld, candle ignite "
                "and attemptIgnite, rug setSkin, jumper cable connect");
    } else if (pending == 0 && sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGW("[SAVE-ACTOR] %d of %d mutator watches are dead -- those changes reach the others at the next join",
                kWatchCount - live, kWatchCount);
    }
}

// ---- births ------------------------------------------------------------------------------------------------------

void HostBirth(Caught& c, void* a) {
    const std::wstring key = SA::EnsureKey(a);
    if (key.empty()) {
        UE_LOGW("[SAVE-ACTOR] host: a %ls minted no key -- it stays the host's alone", ClsName(c.cls));
        return;
    }
    Msg m;
    m.op = kBirth;
    m.owner = 0;
    if (!CaptureBoth(a, c.cls, key, m)) {
        if (++c.tries < kMaxCaptureTries) return;
        UE_LOGW("[SAVE-ACTOR] host: getData refused on %ls '%ls' -- it stays the host's alone", ClsName(c.cls),
                key.c_str());
        return;
    }
    PutRow(key, a, c.cls, 0, true, HashOf(m.better, m.rec));
    ++g_cBorn;
    UE_LOGI("[SAVE-ACTOR] host: %ls '%ls' born -- to every client (record %zu B legacy, %zu B 0.10.0)",
            ClsName(c.cls), key.c_str(), m.rec.names.size(), m.better.size());
    Broadcast(m);
}

void ClientBirth(Caught& c, void* a) {
    if (c.shadow) {
        ++g_cShadowed;
        UE_LOGI("[SAVE-ACTOR] client: a %ls this peer's copy made for itself is retired -- the host's copy makes the "
                "one every peer keeps", ClsName(c.cls));
        DestroyOurs(a);
        return;
    }
    if (g_reqs.size() >= kMaxReqs) {
        UE_LOGW("[SAVE-ACTOR] client: %zu births wait on the host -- this %ls stays local", g_reqs.size(),
                ClsName(c.cls));
        return;
    }
    const std::wstring key = SA::EnsureKey(a);
    if (key.empty()) {
        UE_LOGW("[SAVE-ACTOR] client: a %ls minted no key -- it stays local", ClsName(c.cls));
        return;
    }
    Msg m;
    m.op = kBirth;
    m.owner = coop::players::Registry::Get().LocalPeerId();
    if (!CaptureBoth(a, c.cls, key, m)) {
        if (++c.tries < kMaxCaptureTries) return;
        UE_LOGW("[SAVE-ACTOR] client: getData refused on %ls '%ls' -- it stays local", ClsName(c.cls),
                key.c_str());
        return;
    }
    Req r;
    r.id = g_nextReq++;
    if (g_nextReq == 0) g_nextReq = 1;
    r.ref.Set(a);
    r.key = key;
    r.cls = c.cls;
    r.sentMs = NowMs();
    m.req = r.id;
    // The host's save is the session's; this copy is not to be written into this peer's.
    SA::WriteSkipSave(a, true);
    if (HasPolicy(c.cls, kHostConstraint)) {
        // Its tie is a physics constraint between two props, which only the host may hold: the copy goes now and the
        // host's mirror takes its place.
        r.replaced = true;
        ++g_cReplaced;
        DestroyOurs(a);
    }
    UE_LOGI("[SAVE-ACTOR] client: %ls '%ls' asked of the host as request %u%s", ClsName(c.cls), key.c_str(),
            static_cast<unsigned>(r.id), r.replaced ? " (the host's copy replaces this one)" : "");
    g_reqs.push_back(std::move(r));
    Enqueue(0, Pack(m));
}

void DrainCaught() {
    for (size_t i = 0; i < g_caught.size();) {
        // A whole frame after the finish: the Blueprint's tail has run.
        if (g_tick < g_caught[i].tick + 2) { ++i; continue; }
        // A copy: the birth runs Blueprint (getKey, getData, a destroy) whose own spawns append to g_caught.
        Caught c = g_caught[i];
        void* a = c.ref.Get();
        if (!a) {
            g_caught.erase(g_caught.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const uint8_t before = c.tries;
        if (g_host) HostBirth(c, a);
        else ClientBirth(c, a);
        // A capture to retry next tick; the last try's refusal (logged by the birth) ends it.
        if (c.tries != before && c.tries < kMaxCaptureTries) {
            g_caught[i].tries = c.tries;
            ++i;
            continue;
        }
        g_caught.erase(g_caught.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// ---- deaths ------------------------------------------------------------------------------------------------------

void DrainDeaths() {
    if (g_deaths.empty()) return;
    std::vector<Death> deaths;
    deaths.swap(g_deaths);
    for (Death& d : deaths) {
        g_rows.erase(d.key);
        if (g_host) {
            if (g_tombs.size() >= kMaxTombs) g_tombs.pop_front();
            g_tombs.push_back(d.key);
            Msg m;
            m.op = kDestroy;
            m.key = d.key;
            ++g_cDestroys;
            UE_LOGI("[SAVE-ACTOR] host: %ls '%ls' gone -- to every client", ClsName(d.cls), d.key.c_str());
            Broadcast(m);
            continue;
        }
        // CLIENT: a world load's churn is the save's, and a class that retires itself does so on the host as well.
        if (coop::world_load_episode::InEpisode() || coop::world_load_episode::InReconcileWindow()) continue;
        if (HasPolicy(d.cls, kSelfRetiring)) continue;
        for (size_t i = 0; i < g_reqs.size(); ++i) {
            if (g_reqs[i].key != d.key || g_reqs[i].replaced) continue;
            g_reqs.erase(g_reqs.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
        Msg m;
        m.op = kDestroy;
        m.key = d.key;
        ++g_cDestroys;
        UE_LOGI("[SAVE-ACTOR] client: %ls '%ls' removed here -- asked of the host", ClsName(d.cls),
                d.key.c_str());
        Enqueue(0, Pack(m));
    }
}

// ---- mutators ----------------------------------------------------------------------------------------------------

bool KeyPending(const std::wstring& key) {
    for (const Req& r : g_reqs)
        if (r.key == key) return true;
    return false;
}

void DrainDirty() {
    if (g_dirty.empty()) return;
    std::vector<CachedObjRef> dirty;
    dirty.swap(g_dirty);
    for (const CachedObjRef& d : dirty) {
        void* a = d.Get();
        if (!a) continue;
        const int ci = SA::IndexOf(a);
        if (ci < 0) continue;
        const std::wstring key = SA::KeyOf(a);
        if (key.empty()) continue;
        if (!g_host && KeyPending(key)) continue;   // the echo of its birth compares and sends what changed since
        Msg m;
        m.op = kState;
        if (!CaptureBoth(a, ci, key, m)) continue;
        const uint64_t h = HashOf(m.better, m.rec);
        auto it = g_rows.find(key);
        if (it != g_rows.end() && it->second.hash == h) continue;
        Row& row = PutRow(key, a, ci, it != g_rows.end() ? it->second.owner : uint8_t{0}, false, h);
        ++g_cStates;
        if (g_host) {
            Broadcast(m);
        } else {
            row.localSendMs = NowMs();
            Enqueue(0, Pack(m));
        }
    }
}

// ---- HOST: a client's message ------------------------------------------------------------------------------------

void Refuse(uint8_t slot, uint16_t req, const char* why) {
    ++g_cRefused;
    UE_LOGW("[SAVE-ACTOR] host: slot %u's request %u refused (%s)", static_cast<unsigned>(slot),
            static_cast<unsigned>(req), why);
    Msg m;
    m.op = kRefused;
    m.req = req;
    Enqueue(slot, Pack(m));
}

// The host's view of `actor` to one client: a client whose verb the host would not take gets the actor back.
void ResendBirthTo(uint8_t slot, void* actor, int cls, const std::wstring& key) {
    Msg m;
    m.op = kBirth;
    auto it = g_rows.find(key);
    m.owner = it != g_rows.end() ? it->second.owner : uint8_t{0};
    if (!CaptureBoth(actor, cls, key, m)) return;
    Enqueue(slot, Pack(m));
}

void HostOnBirthIntent(coop::net::Session& s, uint8_t slot, Msg& m) {
    const int ci = SA::IndexOfName(m.rec.className);
    if (ci < 0) return Refuse(slot, m.req, "not an allowlisted class");
    void* cls = SA::ClassAt(static_cast<size_t>(ci));
    if (!cls) return Refuse(slot, m.req, "class not loaded on the host");
    if (!RecordSane(m.rec)) return Refuse(slot, m.req, "a number out of the world");
    const FVector at = LocOf(m.rec);
    const auto tok = coop::element::IntentTarget::ForClientIntent(s, slot, kBirthReachUU);
    const auto sub = tok.AuthorizeSegment(at, at);
    if (sub.outcome == coop::element::IntentOutcome::OutOfReach) return Refuse(slot, m.req, "out of reach");
    // The client's key, when it names nothing here: the host's save keeps the name the client's world already uses.
    const std::wstring asked = m.rec.key;
    std::wstring key = asked;
    if (key.empty() || key == L"None" || g_rows.count(key) || (KO::Available() && KO::Resolve(key.c_str())))
        key = MintKey(slot);
    m.rec.className = ClsName(ci);
    m.rec.key = key;

    ++g_spawning;
    void* a = SA::Spawn(cls, m.rec, key, /*skipSave=*/false);
    if (a) ApplyTo(a, ci, key, m, /*host=*/true);
    --g_spawning;
    if (!a) return Refuse(slot, m.req, "the spawn failed");

    Msg out;
    out.op = kBirth;
    out.owner = slot;
    out.req = m.req;
    if (!CaptureBoth(a, ci, key, out)) {
        out.rec = m.rec;
        out.better = m.better;
    }
    PutRow(key, a, ci, slot, true, HashOf(out.better, out.rec));
    ++g_cSpawnedForClient;
    UE_LOGI("[SAVE-ACTOR] host: slot %u's %ls spawned as '%ls' (request %u%s) -- to every client",
            static_cast<unsigned>(slot), ClsName(ci), key.c_str(), static_cast<unsigned>(m.req),
            key == asked ? "" : ", renamed");
    Broadcast(out);
}

void HostOnStateIntent(coop::net::Session& s, uint8_t slot, Msg& m) {
    int ci = -1;
    const std::wstring key = m.rec.key;
    void* a = key.empty() ? nullptr : FindByKey(key, &ci);
    if (!a) return;
    if (!RecordSane(m.rec)) return;
    const auto sub = coop::element::IntentTarget::ForClientIntent(s, slot, kVerbReachUU).Authorize(a);
    if (sub.outcome == coop::element::IntentOutcome::OutOfReach) {
        UE_LOGW("[SAVE-ACTOR] host: slot %u's change to '%ls' is out of reach -- the host's record goes back",
                static_cast<unsigned>(slot), key.c_str());
        Msg back;
        back.op = kState;
        if (CaptureBoth(a, ci, key, back)) Enqueue(slot, Pack(back));
        return;
    }
    ApplyTo(a, ci, key, m, /*host=*/true);
    Msg out;
    out.op = kState;
    if (!CaptureBoth(a, ci, key, out)) return;
    auto it = g_rows.find(key);
    PutRow(key, a, ci, it != g_rows.end() ? it->second.owner : slot, false, HashOf(out.better, out.rec));
    ++g_cStates;
    Broadcast(out);
}

void HostOnDestroyIntent(coop::net::Session& s, uint8_t slot, const Msg& m) {
    int ci = -1;
    void* a = m.key.empty() ? nullptr : FindByKey(m.key, &ci);
    if (!a) return;   // gone already, or never here: nothing to judge
    const auto sub = coop::element::IntentTarget::ForClientIntent(s, slot, kVerbReachUU).Authorize(a);
    if (sub.outcome == coop::element::IntentOutcome::OutOfReach) {
        UE_LOGW("[SAVE-ACTOR] host: slot %u's removal of '%ls' is out of reach (%.0f > %.0f) -- it goes back to it",
                static_cast<unsigned>(slot), m.key.c_str(), sub.distUU, sub.reachUU);
        ResendBirthTo(slot, a, ci, m.key);
        return;
    }
    UE_LOGI("[SAVE-ACTOR] host: slot %u removed %ls '%ls'", static_cast<unsigned>(slot), ClsName(ci),
            m.key.c_str());
    E::DestroyActor(a);   // its end of play announces it to every client
}

// ---- CLIENT: the host's message ----------------------------------------------------------------------------------

// Spawn or adopt the host's actor for a birth. False when it must wait (the class is not loaded, or the key table
// cannot say whether the save already gave it).
bool ClientPlace(const Msg& m) {
    const int ci = SA::IndexOfName(m.rec.className);
    const std::wstring& key = m.rec.key;
    if (ci < 0 || key.empty() || !RecordSane(m.rec)) return true;   // nothing this peer can make of it
    const uint64_t h = HashOf(m.better, m.rec);

    // Already here: the row's, or the save's copy, which a joiner adopts instead of spawning a second.
    auto it = g_rows.find(key);
    void* have = it != g_rows.end() ? it->second.ref.Get() : nullptr;
    if (!have) {
        if (!KO::Available()) return false;
        have = KO::Resolve(key.c_str());
        if (have && SA::IndexOf(have) != ci) have = nullptr;
        if (have) {
            SA::WriteSkipSave(have, true);
            ++g_cAdopted;
        }
    }
    if (have) {
        ApplyTo(have, ci, key, m, /*host=*/false);
        PutRow(key, have, ci, m.owner, false, h);
        return true;
    }
    void* cls = SA::ClassAt(static_cast<size_t>(ci));
    if (!cls) return false;
    ++g_spawning;
    void* a = SA::Spawn(cls, m.rec, key, /*skipSave=*/true);
    if (a) ApplyTo(a, ci, key, m, /*host=*/false);
    --g_spawning;
    if (!a) {
        UE_LOGW("[SAVE-ACTOR] client: the mirror of %ls '%ls' did not spawn", ClsName(ci), key.c_str());
        return true;
    }
    PutRow(key, a, ci, m.owner, false, h);
    ++g_cMirrors;
    UE_LOGI("[SAVE-ACTOR] client: %ls '%ls' mirrored (slot %u's)", ClsName(ci), key.c_str(),
            static_cast<unsigned>(m.owner));
    return true;
}

// The echo of this client's own birth: its copy is bound under the host's key. True when the message was that echo.
bool ClientBindOwn(const Msg& m) {
    if (m.req == 0 || m.owner != coop::players::Registry::Get().LocalPeerId()) return false;
    for (size_t i = 0; i < g_reqs.size(); ++i) {
        if (g_reqs[i].id != m.req) continue;
        Req r = std::move(g_reqs[i]);
        g_reqs.erase(g_reqs.begin() + static_cast<std::ptrdiff_t>(i));
        void* a = r.replaced ? nullptr : r.ref.Get();
        if (!a) return false;   // replaced, or gone meanwhile: the host's copy is mirrored as any other
        const std::wstring& key = m.rec.key;
        if (key != r.key) {
            SA::WriteKey(a, key);
            SA::EnsureKey(a);
        }
        const int ci = r.cls;
        PutRow(key, a, ci, m.owner, false, HashOf(m.better, m.rec));
        ++g_cBound;
        UE_LOGI("[SAVE-ACTOR] client: request %u answered -- this copy is '%ls'%s", static_cast<unsigned>(r.id),
                key.c_str(), key != r.key ? " (renamed by the host)" : "");
        // What changed here since the birth was captured goes now.
        CachedObjRef d;
        d.Set(a);
        if (g_dirty.size() < kMaxDirty) g_dirty.push_back(d);
        return true;
    }
    return false;
}

void ClientOnBirth(Msg& m) {
    if (ClientBindOwn(m)) return;
    if (ClientPlace(m)) return;
    if (g_parked.size() >= kMaxParked) {
        ++g_cDropped;
        return;
    }
    g_parked.push_back(Parked{std::move(m), NowMs() + kParkMs});
}

void ClientOnState(const Msg& m) {
    int ci = -1;
    const std::wstring& key = m.rec.key;
    void* a = key.empty() ? nullptr : FindByKey(key, &ci);
    if (!a || !RecordSane(m.rec)) return;
    const uint64_t h = HashOf(m.better, m.rec);
    auto it = g_rows.find(key);
    if (it != g_rows.end() && NowMs() - it->second.localSendMs < kEchoQuietMs) {
        // This peer changed it a moment ago; the host's answer to that change follows this one.
        it->second.hash = h;
        return;
    }
    if (it != g_rows.end() && it->second.hash == h) return;
    ApplyTo(a, ci, key, m, /*host=*/false);
    PutRow(key, a, ci, it != g_rows.end() ? it->second.owner : uint8_t{0}, false, h);
}

void ClientOnDestroy(const Msg& m) {
    for (size_t i = 0; i < g_parked.size();) {
        if (g_parked[i].m.rec.key == m.key) g_parked.erase(g_parked.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
    void* a = m.key.empty() ? nullptr : FindByKey(m.key);
    g_rows.erase(m.key);
    if (!a) return;
    DestroyOurs(a);
}

void ClientOnRefused(const Msg& m) {
    for (size_t i = 0; i < g_reqs.size(); ++i) {
        if (g_reqs[i].id != m.req) continue;
        void* a = g_reqs[i].ref.Get();
        UE_LOGW("[SAVE-ACTOR] client: the host refused request %u (%ls '%ls') -- this copy is retired",
                static_cast<unsigned>(m.req), ClsName(g_reqs[i].cls), g_reqs[i].key.c_str());
        g_reqs.erase(g_reqs.begin() + static_cast<std::ptrdiff_t>(i));
        ++g_cRefused;
        if (a) DestroyOurs(a);
        return;
    }
}

void ClientRetry() {
    const uint64_t now = NowMs();
    for (size_t i = 0; i < g_parked.size();) {
        if (ClientPlace(g_parked[i].m)) {
            g_parked.erase(g_parked.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (now >= g_parked[i].untilMs) {
            UE_LOGW("[SAVE-ACTOR] client: %ls '%ls' waited %llu ms for its class -- dropped",
                    g_parked[i].m.rec.className.c_str(), g_parked[i].m.rec.key.c_str(),
                    static_cast<unsigned long long>(kParkMs));
            ++g_cDropped;
            g_parked.erase(g_parked.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
    for (size_t i = 0; i < g_reqs.size();) {
        if (now - g_reqs[i].sentMs < kReqTimeoutMs) { ++i; continue; }
        UE_LOGW("[SAVE-ACTOR] client: request %u (%ls '%ls') had no answer in %llu ms -- the copy stays local",
                static_cast<unsigned>(g_reqs[i].id), ClsName(g_reqs[i].cls), g_reqs[i].key.c_str(),
                static_cast<unsigned long long>(kReqTimeoutMs));
        g_reqs.erase(g_reqs.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

coop::blob_chunks::Assembler g_asmIntent, g_asmState;

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!session) return;
    if (g_keyNonce == 0) {
        std::random_device rd;
        g_keyNonce = static_cast<uint32_t>(rd()) | 1u;
    }
    ComputePolicies();
    InstallSeam();
    InstallSink();
    RegisterWatches();
}

void Tick(coop::net::Session& session) {
    ++g_tick;
    if (!g_settled) {
        sg::ResolvePendingNames();
        Settle();
    }
    const uint64_t now = NowMs();
    if (now >= g_nextRefreshMs) {
        g_nextRefreshMs = now + 1000;
        SA::Refresh();
    }
    g_host = session.role() == coop::net::Role::Host;
    g_live = session.running() && (g_host || session.connected());
    if (!g_live) {
        g_caught.clear();
        g_deaths.clear();
        g_dirty.clear();
        return;
    }
    DrainCaught();
    DrainDeaths();
    DrainDirty();
    if (!g_host) ClientRetry();
    DrainOut(session);
    if (!g_asmIntent.Idle() || !g_asmState.Idle()) {
        static uint64_t s_nextSweep = 0;
        if (now >= s_nextSweep) {
            s_nextSweep = now + 1000;
            g_asmIntent.Sweep(std::chrono::steady_clock::now(), std::chrono::seconds(20));
            g_asmState.Sweep(std::chrono::steady_clock::now(), std::chrono::seconds(20));
        }
    }
}

void OnPeerWorldReady(int slot) {
    auto* s = Session();
    if (!s || s->role() != coop::net::Role::Host || slot < 1 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    int destroys = 0, births = 0;
    for (const std::wstring& k : g_tombs) {
        Msg m;
        m.op = kDestroy;
        m.key = k;
        Enqueue(slot, Pack(m));
        ++destroys;
    }
    for (auto& [key, row] : g_rows) {
        void* a = row.ref.Get();
        if (!a || !row.born) continue;
        Msg m;
        m.op = kBirth;
        m.owner = row.owner;
        m.flags = kFlagSnapshot;
        if (!CaptureBoth(a, row.cls, key, m)) continue;
        Enqueue(slot, Pack(m));
        ++births;
    }
    if (destroys || births)
        UE_LOGI("[SAVE-ACTOR] host: slot %d's world is up -- %d removal(s) and %d birth(s) of this session queued", slot,
                destroys, births);
}

void OnIntentChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    auto* s = Session();
    if (!GT::IsGameThread() || !s || s->role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    std::vector<uint8_t> blob;
    if (!g_asmIntent.OnChunk(p, senderSlot, blob)) return;
    Msg m;
    if (!Parse(blob, m) || m.op == kRefused) {
        ++g_cDropped;
        UE_LOGW("[SAVE-ACTOR] host: a malformed SaveActorIntent (%zu B) from slot %u dropped", blob.size(),
                static_cast<unsigned>(senderSlot));
        return;
    }
    if (!SpendBudget(senderSlot)) {
        ++g_cDropped;
        if (m.op == kBirth) Refuse(senderSlot, m.req, "over budget");
        return;
    }
    switch (m.op) {
        case kBirth: HostOnBirthIntent(*s, senderSlot, m); break;
        case kState: HostOnStateIntent(*s, senderSlot, m); break;
        case kDestroy: HostOnDestroyIntent(*s, senderSlot, m); break;
        default: break;
    }
}

void OnStateChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    auto* s = Session();
    if (!GT::IsGameThread() || !s || s->role() != coop::net::Role::Client || senderSlot != 0) return;
    std::vector<uint8_t> blob;
    if (!g_asmState.OnChunk(p, senderSlot, blob)) return;
    Msg m;
    if (!Parse(blob, m)) {
        ++g_cDropped;
        UE_LOGW("[SAVE-ACTOR] client: a malformed SaveActorState (%zu B) dropped", blob.size());
        return;
    }
    switch (m.op) {
        case kBirth: ClientOnBirth(m); break;
        case kState: ClientOnState(m); break;
        case kDestroy: ClientOnDestroy(m); break;
        case kRefused: ClientOnRefused(m); break;
        default: break;
    }
}

void OnPeerLeft(uint8_t slot) {
    if (slot >= coop::net::kMaxPeers) return;
    // Slots recycle lowest-free, so a rejoining peer's chunks must never merge into the previous occupant's half.
    g_asmIntent.ClearSlot(slot);
    g_budget[slot] = Budget{};
    // A message queued for the leaver never sends (the slot refuses it), and the queue is all-or-nothing at its head:
    // left there, it would hold every later message back for good.
    for (auto it = g_out.begin(); it != g_out.end();) {
        if (it->slot == static_cast<int>(slot)) it = g_out.erase(it);
        else ++it;
    }
}

void OnDisconnect() {
    UE_LOGI("[SAVE-ACTOR] session ended: born %llu, spawned for clients %llu, mirrors %llu, adopted %llu, bound %llu, "
            "states %llu, removals %llu, refused %llu, retired copies %llu, replaced copies %llu, local-only births "
            "%llu, dropped %llu, rows %zu",
            static_cast<unsigned long long>(g_cBorn), static_cast<unsigned long long>(g_cSpawnedForClient),
            static_cast<unsigned long long>(g_cMirrors), static_cast<unsigned long long>(g_cAdopted),
            static_cast<unsigned long long>(g_cBound), static_cast<unsigned long long>(g_cStates),
            static_cast<unsigned long long>(g_cDestroys), static_cast<unsigned long long>(g_cRefused),
            static_cast<unsigned long long>(g_cShadowed), static_cast<unsigned long long>(g_cReplaced),
            static_cast<unsigned long long>(g_cLocalOnly), static_cast<unsigned long long>(g_cDropped), g_rows.size());
    g_live = false;
    g_rows.clear();
    g_tombs.clear();
    g_caught.clear();
    g_deaths.clear();
    g_incoming.clear();
    g_dirty.clear();
    g_reqs.clear();
    g_parked.clear();
    g_out.clear();
    for (auto& b : g_budget) b = Budget{};
    g_asmIntent.Clear();
    g_asmState.Clear();
    g_nextReq = 1;
    g_spawning = g_applying = 0;
    g_outFullSaid = false;
    g_cBorn = g_cSpawnedForClient = g_cMirrors = g_cAdopted = g_cBound = g_cStates = 0;
    g_cDestroys = g_cRefused = g_cShadowed = g_cLocalOnly = g_cReplaced = g_cDropped = 0;
    SA::ResetCache();
}

}  // namespace coop::save_actor_sync
