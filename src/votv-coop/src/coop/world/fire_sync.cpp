// coop/world/fire_sync.cpp -- see coop/world/fire_sync.h.

#include "coop/world/fire_sync.h"

#include "coop/element/object_scan_hub.h"
#include "coop/interactables/grime_sync.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/world/spawn_authority.h"
#include "coop/world/world_actor_sync.h"

#include "ue_wrap/actors/fire.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::fire_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace F  = ue_wrap::fire;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace SG = ue_wrap::script_gate;
namespace P  = ue_wrap::profile;

std::atomic<coop::net::Session*> g_session{nullptr};
coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

bool IsHost() {
    coop::net::Session* s = LoadSession();
    return s && s->running() && s->role() == coop::net::Role::Host;
}

bool IsClient() { return coop::spawn_authority::IsActiveClientSession(); }

using Clock = std::chrono::steady_clock;

// ---- the watches ----------------------------------------------------------------------------------------------------

UE_FACT_FN(kRowFireBeginPlay, "fire_sync", L"fire_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowSpread, "fire_sync", L"comp_flammable_C", L"spreadFire", Declared);
UE_FACT_FN(kRowGasIgnite, "fire_sync", L"grime_gasoline_C", L"ignite", Declared);
constexpr int kTagBeginPlay = 0x46495242;  // 'FIRB'
constexpr int kTagSpread    = 0x46495253;  // 'FIRS'
constexpr int kTagGasIgnite = 0x46495247;  // 'FIRG'

// One watch's install: registered once, then asked until it is live or settled dead, and said once either way.
struct WatchState {
    const ue_wrap::facts::Function* row;
    int         tag;
    SG::PreFn   pre;
    SG::PostFn  post;
    const char* what;
    bool        registered = false;
    bool        settled = false;
    int         refusals = 0;
};

// ---- counters -------------------------------------------------------------------------------------------------------

struct Counts {
    uint32_t enrolled = 0, enrollFailed = 0, sweptEnrolled = 0;
    uint32_t dissolved = 0, sweptRemoved = 0, spreadCancelled = 0;
    uint32_t mirrorsSeeded = 0, mirrorsDefaulted = 0;
    uint32_t puddlesLit = 0, puddlesOut = 0, puddleIgniteCancelled = 0;
    uint32_t flameApplied = 0, flameOff = 0, flameExpired = 0, flameNotPuddle = 0;
};
Counts g_counts{};
constexpr uint32_t kSaid = 8;

// ---- client: a fire this machine's own game starts ------------------------------------------------------------------

void DestroyLater(void* actor) {
    const int32_t idx = R::InternalIndexOf(actor);
    GT::Post([actor, idx] {
        if (R::IsLiveByIndex(actor, idx)) E::DestroyActor(actor);
    });
}

// fire_C's begin-play on a client: the lane's own mirror runs it (its parameters were seeded parked); any other is
// this machine's own fire -- a cigarette, a bomb, a spreading flame or a save's load minted it -- and is cancelled and
// destroyed. The host's fire reaches this client as its mirror.
SG::Verdict OnFireBeginPlayPre(const SG::Call& c) {
    if (!c.object || !IsClient()) return SG::Verdict::Run;
    if (coop::world_actor_sync::IsMaterializingMirror() && coop::world_actor_sync::MaterializingActor() == c.object)
        return SG::Verdict::Run;
    DestroyLater(c.object);
    const uint32_t n = ++g_counts.dissolved;
    if (n <= kSaid) {
        ue_wrap::FVector at{};
        E::TryGetActorLocation(c.object, at);
        UE_LOGI("[FIRE] client: a fire this machine started at (%.0f,%.0f,%.0f) is the host's call -- cancelled and "
                "destroyed (#%u%s)", at.X, at.Y, at.Z, n, n == kSaid ? "; the rest are counted" : "");
    }
    return SG::Verdict::Cancel;
}

// The host's fire, enrolled on the world-actor lane: it allocates the element, binds the actor and broadcasts the spawn
// with the birth blob (FillBirth). A second call for one actor returns its element and sends nothing.
void Enroll(void* actor, const char* why) {
    if (coop::world_actor_sync::HostEidOf(actor) != 0) return;
    const unsigned int eid = coop::world_actor_sync::HostEnrollExSpawn(actor);
    if (eid == 0) {
        const uint32_t n = ++g_counts.enrollFailed;
        if (n <= kSaid) UE_LOGW("[FIRE] host: a fire (%s) was not enrolled on the world-actor lane (#%u)", why, n);
        return;
    }
    const uint32_t n = ++g_counts.enrolled;
    if (n <= kSaid || (n % 50) == 0) UE_LOGI("[FIRE] host: fire eid=%u enrolled at its %s (#%u)", eid, why, n);
}

// The host's fire after its begin-play: its component is lit and its fields are read. A fire its begin-play put out
// (under water) is already going and is left alone.
void OnFireBeginPlayPost(const SG::Call& c) {
    if (!c.object || !IsHost() || !R::IsLive(c.object)) return;
    Enroll(c.object, "begin-play");
}

// comp_flammable_C's spread on a client, on a mirrored fire or a mirrored puddle's flame: every client fire is the
// host's mirror, so its spread (the owner's fireDamage, attemptIgnite on what it touches, a flammable floor lit) is the
// host's alone.
SG::Verdict OnSpreadPre(const SG::Call& c) {
    if (!c.object || !IsClient()) return SG::Verdict::Run;
    void* owner = F::FlammableOwner(c.object);
    if (!owner || !(F::IsFire(owner) || F::IsGasolineGrime(owner))) return SG::Verdict::Run;
    const uint32_t n = ++g_counts.spreadCancelled;
    if (n == 1)
        UE_LOGI("[FIRE] client: a mirrored flame's spread is the host's -- cancelled (said once; counted)");
    return SG::Verdict::Cancel;
}

// ---- the puddles ----------------------------------------------------------------------------------------------------

// The depth of this lane's own ignite or extinguish of a puddle on a client: the one scope the puddle's ignite runs in.
int g_laneDepth = 0;

// HOST: the puddles burning here, read twice a second for their end.
struct Burning {
    void*        actor;
    int32_t      idx;
    int32_t      serial;
    std::wstring key;
    float        fuel;
};
std::vector<Burning> g_burning;
constexpr size_t kMaxBurning = 64;
Clock::time_point g_nextHostRead{};

// The fuel of the ignite whose body is running, read at its entry, and the puddle it runs on. An ignite never nests
// on one puddle.
R::ParamOffset g_igniteFuelParam{P::name::GasolineIgniteFuelParam};
void* g_igniting = nullptr;
float g_igniteFuel = 0.f;

void SendFlame(int slot, const std::wstring& key, bool on, float fuel) {
    coop::net::Session* s = LoadSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    coop::net::KeyedScalarPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.value = on ? fuel : 0.f;
    p.adopt = on ? coop::net::kGrimeOpFireOn : coop::net::kGrimeOpFireOff;
    const bool ok = slot < 0
        ? s->SendReliable(coop::net::ReliableKind::GrimeState, &p, static_cast<int>(sizeof(p)))
        : s->SendReliableToSlot(slot, coop::net::ReliableKind::GrimeState, &p, static_cast<int>(sizeof(p)));
    if (!ok) UE_LOGW("[FIRE] host: the puddle flame %s for key='%ls' was not sent", on ? "on" : "off", key.c_str());
}

bool Holds(const Burning& b) {
    return R::IsLiveByIndex(b.actor, b.idx) && R::SlotSerial(b.idx) == b.serial;
}

SG::Verdict OnGasIgnitePre(const SG::Call& c) {
    g_igniting = nullptr;
    if (!c.object) return SG::Verdict::Run;
    if (IsClient()) {
        if (g_laneDepth > 0) return SG::Verdict::Run;
        const uint32_t n = ++g_counts.puddleIgniteCancelled;
        if (n <= kSaid)
            UE_LOGI("[FIRE] client: a fuel puddle this machine lit is the host's call -- cancelled (#%u)", n);
        return SG::Verdict::Cancel;
    }
    if (!IsHost()) return SG::Verdict::Run;
    g_igniting = c.object;
    g_igniteFuel = 0.f;
    const int32_t off = c.locals ? g_igniteFuelParam.Of(c.function) : -1;
    if (off >= 0) std::memcpy(&g_igniteFuel, c.locals + off, sizeof(g_igniteFuel));
    return SG::Verdict::Run;
}

// The host's puddle after its ignite: newly burning, it is tracked and sent.
void OnGasIgnitePost(const SG::Call& c) {
    if (!c.object || c.object != g_igniting) return;
    g_igniting = nullptr;
    if (!IsHost() || !R::IsLive(c.object)) return;
    void* comp = F::GasolineFlameBase(c.object);
    if (!comp || !F::FlammableBurning(comp)) return;
    // Already tracked: a top-up of a burning puddle is not a new flame. One that burned before this ignite and is not
    // tracked was lit before the session could see it, and is tracked now.
    for (const Burning& b : g_burning)
        if (b.actor == c.object && Holds(b)) return;
    const std::wstring key = coop::grime_sync::KeyForDecal(c.object);
    if (key.empty()) {
        UE_LOGW("[FIRE] host: a lit fuel puddle has no grime key (its location could not be read) -- not sent");
        return;
    }
    if (g_burning.size() >= kMaxBurning) {
        static bool sFull = false;
        if (!sFull) {
            sFull = true;
            UE_LOGW("[FIRE] host: %zu fuel puddles already burn -- this one is not sent (said once)", kMaxBurning);
        }
        return;
    }
    const int32_t idx = R::InternalIndexOf(c.object);
    float fuel = g_igniteFuel;
    if (!std::isfinite(fuel) || fuel < 0.f) fuel = 0.f;
    g_burning.push_back(Burning{c.object, idx, R::AllocateSlotSerial(idx), key, fuel});
    ++g_counts.puddlesLit;
    UE_LOGI("[FIRE] host: fuel puddle key='%ls' lit fuel=%.1f -- sent", key.c_str(), fuel);
    SendFlame(-1, key, true, fuel);
}

// HOST: a tracked puddle whose flame is gone (put out, burnt out, the decal destroyed) is sent off and dropped.
void HostReadPuddles() {
    if (g_burning.empty()) return;
    const Clock::time_point now = Clock::now();
    if (now < g_nextHostRead) return;
    g_nextHostRead = now + std::chrono::milliseconds(500);
    for (size_t i = 0; i < g_burning.size();) {
        Burning& b = g_burning[i];
        const bool held = Holds(b);
        void* comp = held ? F::GasolineFlameBase(b.actor) : nullptr;
        if (comp && F::FlammableBurning(comp)) {
            ++i;
            continue;
        }
        ++g_counts.puddlesOut;
        UE_LOGI("[FIRE] host: fuel puddle key='%ls' out (%s) -- sent", b.key.c_str(),
                held ? "its flame ended" : "the decal is gone");
        SendFlame(-1, b.key, false, 0.f);
        g_burning[i] = std::move(g_burning.back());
        g_burning.pop_back();
    }
}

// CLIENT: the host's lit puddles this machine has not lit yet (its index has not reached the key), by key.
struct Waiting {
    float             fuel;
    Clock::time_point until;
};
std::unordered_map<std::wstring, Waiting> g_waiting;
constexpr size_t kMaxWaiting = 64;
Clock::time_point g_nextRetry{};

// Light the client's copy of a puddle the host lit, and park its component. True when done or not to be retried.
bool TryLight(const std::wstring& key, float fuel) {
    void* decal = coop::grime_sync::ResolveDecal(key);
    if (!decal) return false;
    if (!F::IsGasolineGrime(decal)) {
        const uint32_t n = ++g_counts.flameNotPuddle;
        if (n <= kSaid)
            UE_LOGW("[FIRE] client: key='%ls' names a %ls here, not a fuel puddle -- the flame is dropped", key.c_str(),
                    R::ClassNameOf(decal).c_str());
        return true;
    }
    void* comp = F::GasolineFlameBase(decal);
    if (!comp || !F::FlammableBurning(comp)) {
        ++g_laneDepth;
        const bool ran = F::CallGasolineIgnite(decal, fuel);
        --g_laneDepth;
        comp = F::GasolineFlameBase(decal);
        if (!ran || !comp) {
            UE_LOGW("[FIRE] client: fuel puddle key='%ls' -- its ignite %s; not lit", key.c_str(),
                    ran ? "left no flame" : "could not run");
            return true;
        }
    }
    const bool parked = F::ParkFlammable(comp, 0.f, 0.f);
    ++g_counts.flameApplied;
    UE_LOGI("[FIRE] client: fuel puddle key='%ls' lit with the host's (fuel=%.1f)%s", key.c_str(), fuel,
            parked ? "" : " -- its burn could not be parked; it may go out here before the host's");
    return true;
}

void PutOut(const std::wstring& key) {
    void* decal = coop::grime_sync::ResolveDecal(key);
    if (!decal || !F::IsGasolineGrime(decal)) return;
    void* comp = F::GasolineFlameBase(decal);
    if (!comp || !F::FlammableBurning(comp)) return;
    ++g_laneDepth;
    const bool ran = F::CallExtinguish(comp);
    --g_laneDepth;
    ++g_counts.flameOff;
    UE_LOGI("[FIRE] client: fuel puddle key='%ls' out with the host's%s", key.c_str(),
            ran ? "" : " -- its extinguish could not run");
}

void RetryWaiting() {
    if (g_waiting.empty()) return;
    const Clock::time_point now = Clock::now();
    if (now < g_nextRetry) return;
    g_nextRetry = now + std::chrono::seconds(1);
    for (auto it = g_waiting.begin(); it != g_waiting.end();) {
        if (TryLight(it->first, it->second.fuel)) {
            it = g_waiting.erase(it);
            continue;
        }
        if (now >= it->second.until) {
            ++g_counts.flameExpired;
            UE_LOGW("[FIRE] client: fuel puddle key='%ls' named no decal here for two minutes -- the host's flame is "
                    "not shown", it->first.c_str());
            it = g_waiting.erase(it);
            continue;
        }
        ++it;
    }
}

// ---- the sweep ------------------------------------------------------------------------------------------------------

// The scan hub's fire_C walk: on the host, a fire that began play before the lane could see it (a save's load before
// hosting, a watch not yet live) is enrolled; on a client, a fire that is not a mirror (one that began play before the
// session or the watch) is destroyed.
struct Seen {
    void*   actor;
    int32_t idx;
};
std::vector<Seen> g_seen;

bool HubResolved() { return true; }
bool HubIsInstance(void* obj) { return F::IsFireClass(R::ClassOf(obj)); }
void HubPassBegin(void*, bool) { g_seen.clear(); }
void HubMatch(void*, void* obj) {
    if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
    if (!R::IsLive(obj)) return;
    g_seen.push_back(Seen{obj, R::InternalIndexOf(obj)});
}
size_t HubPassComplete(void*, bool, uint32_t) {
    const size_t n = g_seen.size();
    const bool host = IsHost();
    const bool client = IsClient();
    for (const Seen& f : g_seen) {
        if (!R::IsLiveByIndex(f.actor, f.idx)) continue;
        if (host) {
            if (coop::world_actor_sync::HostEidOf(f.actor) == 0) {
                ++g_counts.sweptEnrolled;
                Enroll(f.actor, "sweep");
            }
        } else if (client && !coop::world_actor_sync::IsMirroredActor(f.actor)) {
            E::DestroyActor(f.actor);
            const uint32_t c = ++g_counts.sweptRemoved;
            if (c <= kSaid) UE_LOGI("[FIRE] client: a fire that is not the host's mirror removed by the sweep (#%u)", c);
        }
    }
    g_seen.clear();
    return n;
}

void RegisterWithScanHub() {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
        "fire", nullptr, &HubResolved, &HubIsInstance, &HubPassBegin, &HubMatch, &HubPassComplete,
        /*settleScans*/ 2});
}

// ---- the watch table ------------------------------------------------------------------------------------------------

WatchState g_watches[] = {
    {&kRowFireBeginPlay, kTagBeginPlay, &OnFireBeginPlayPre, &OnFireBeginPlayPost,
     "fire_C.ReceiveBeginPlay (the host's enrol, a client's own fire cancelled)"},
    {&kRowSpread, kTagSpread, &OnSpreadPre, nullptr, "comp_flammable_C.spreadFire (a client mirror's spread parked)"},
    {&kRowGasIgnite, kTagGasIgnite, &OnGasIgnitePre, &OnGasIgnitePost,
     "grime_gasoline_C.ignite (the host's puddle flame; a client's own cancelled)"},
};

void InstallWatches() {
    bool anyRegistered = false;
    for (WatchState& w : g_watches) {
        if (w.settled) continue;
        if (!w.registered) {
            if (SG::WatchName(*w.row, w.tag, w.pre, w.post)) {
                w.registered = true;
            } else if (++w.refusals == 30) {
                w.settled = true;
                UE_LOGE("[FIRE] the watch on %s was refused 30 times -- it is not live", w.what);
            }
        }
        if (w.registered) anyRegistered = true;
    }
    if (!anyRegistered) return;
    SG::ResolvePendingNames();
    for (WatchState& w : g_watches) {
        if (!w.registered || w.settled) continue;
        if (SG::NameWatchLive(*w.row, w.tag)) {
            w.settled = true;
            UE_LOGI("[FIRE] watch live: %s", w.what);
        } else if (SG::NameWatchSettled(*w.row, w.tag)) {
            w.settled = true;
            UE_LOGE("[FIRE] the watch on %s settled DEAD in the script gate -- that half of the fire lane is off",
                    w.what);
        }
    }
}

bool AllFinite(const float* v, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}

constexpr uint8_t kBirthVersion = 1;
constexpr size_t  kBirthLen = 1 + 8 * sizeof(float);

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWithScanHub();
}

void Tick() {
    RegisterWithScanHub();
    InstallWatches();
    if (IsHost()) HostReadPuddles();
    else if (IsClient()) RetryWaiting();
}

void OnDisconnect() {
    const Counts& c = g_counts;
    if (c.enrolled || c.dissolved || c.sweptRemoved || c.spreadCancelled || c.mirrorsSeeded || c.puddlesLit ||
        c.flameApplied || c.puddleIgniteCancelled)
        UE_LOGI("[FIRE] session end: host enrolled=%u (sweep %u, failed %u), puddles lit=%u out=%u; client "
                "dissolved=%u swept=%u spread-cancelled=%u mirrors seeded=%u defaulted=%u, puddle ignites "
                "cancelled=%u, flames lit=%u out=%u expired=%u not-a-puddle=%u",
                c.enrolled, c.sweptEnrolled, c.enrollFailed, c.puddlesLit, c.puddlesOut, c.dissolved, c.sweptRemoved,
                c.spreadCancelled, c.mirrorsSeeded, c.mirrorsDefaulted, c.puddleIgniteCancelled, c.flameApplied,
                c.flameOff, c.flameExpired, c.flameNotPuddle);
    g_counts = Counts{};
    g_burning.clear();
    g_waiting.clear();
    g_igniting = nullptr;
    g_laneDepth = 0;
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    if (!IsHost() || peerSlot < 1) return;
    int sent = 0;
    for (const Burning& b : g_burning) {
        if (!Holds(b)) continue;
        void* comp = F::GasolineFlameBase(b.actor);
        if (!comp || !F::FlammableBurning(comp)) continue;  // the next read sends it off to everyone
        SendFlame(peerSlot, b.key, true, b.fuel);
        ++sent;
    }
    if (sent > 0) UE_LOGI("[FIRE] host: connect-snapshot -- %d burning fuel puddle(s) to slot %d", sent, peerSlot);
}

bool IsFireActor(void* actor) { return actor && F::IsFire(actor); }

bool IsFireClassName(const std::wstring& cls) { return _wcsicmp(cls.c_str(), P::name::FireClass) == 0; }

void FillBirth(void* actor, unsigned int eid, uint8_t* blob, size_t cap, uint8_t& len) {
    len = 0;
    F::Params p{};
    if (!blob || cap < kBirthLen || !F::ReadParams(actor, p)) {
        UE_LOGW("[FIRE] host: fire eid=%u -- its fields could not be read; its mirror is born at the class defaults",
                eid);
        return;
    }
    const float v[8] = {p.fuel, p.fuelSpeed, p.fireSize, p.effectRate, p.igniteChance,
                        p.boxExtent.X, p.boxExtent.Y, p.boxExtent.Z};
    blob[0] = kBirthVersion;
    std::memcpy(blob + 1, v, sizeof(v));
    len = static_cast<uint8_t>(kBirthLen);
}

void SeedMirror(void* actor, unsigned int eid, const uint8_t* blob, size_t len) {
    if (!actor) return;
    F::Params p{};
    bool fromHost = false;
    if (blob && len == kBirthLen && blob[0] == kBirthVersion) {
        float v[8];
        std::memcpy(v, blob + 1, sizeof(v));
        // The fields go to the game's own math: finite and in the ranges its producers use, else the defaults.
        const bool ok = AllFinite(v, 8) && v[0] >= 0.f && v[0] <= 10000.f && v[2] >= 0.f && v[2] <= 1000.f &&
                        v[3] >= 0.f && v[3] <= 1000.f && v[5] >= 0.f && v[5] <= 10000.f && v[6] >= 0.f &&
                        v[6] <= 10000.f && v[7] >= 0.f && v[7] <= 10000.f;
        if (ok) {
            p = F::Params{v[0], v[1], v[2], v[3], v[4], ue_wrap::FVector{v[5], v[6], v[7]}};
            fromHost = true;
        } else {
            UE_LOGW("[FIRE] client: fire eid=%u -- its birth fields are out of range; the class defaults are used", eid);
        }
    } else if (len != 0) {
        UE_LOGW("[FIRE] client: fire eid=%u -- birth blob of %zu bytes (version %u), expected %zu (version %u); the "
                "class defaults are used", eid, len, blob ? static_cast<unsigned>(blob[0]) : 0u, kBirthLen,
                static_cast<unsigned>(kBirthVersion));
    }
    if (!fromHost && !F::ReadParams(actor, p)) {
        UE_LOGW("[FIRE] client: fire eid=%u -- its fields do not resolve; the mirror is not parked and may spread", eid);
        return;
    }
    // Parked: the component its begin-play makes burns no fuel here (the host's destroy ends it), and its spread is
    // cancelled anyway.
    p.fuelSpeed = 0.f;
    p.igniteChance = 0.f;
    if (!F::WriteParams(actor, p)) {
        UE_LOGW("[FIRE] client: fire eid=%u -- its fields could not be written; the mirror is not parked", eid);
        return;
    }
    if (fromHost) ++g_counts.mirrorsSeeded;
    else ++g_counts.mirrorsDefaulted;
    const uint32_t n = g_counts.mirrorsSeeded + g_counts.mirrorsDefaulted;
    if (n <= kSaid || (n % 50) == 0)
        UE_LOGI("[FIRE] client: fire eid=%u mirrored (%s) fuel=%.1f size=%.2f box=(%.0f,%.0f,%.0f), parked", eid,
                fromHost ? "the host's fields" : "class defaults", p.fuel, p.fireSize, p.boxExtent.X, p.boxExtent.Y,
                p.boxExtent.Z);
}

void OnGasolineFlame(const std::wstring& key, bool on, float fuel) {
    if (!IsClient() || key.empty()) return;
    if (!on) {
        g_waiting.erase(key);
        PutOut(key);
        return;
    }
    if (!std::isfinite(fuel) || fuel < 0.f) fuel = 0.f;
    if (fuel > 60.f) fuel = 60.f;  // ignite_s keeps at most 60
    if (TryLight(key, fuel)) {
        g_waiting.erase(key);
        return;
    }
    if (g_waiting.size() >= kMaxWaiting && g_waiting.find(key) == g_waiting.end()) {
        UE_LOGW("[FIRE] client: %zu puddle flames already wait for their decal -- key='%ls' dropped", kMaxWaiting,
                key.c_str());
        return;
    }
    g_waiting[key] = Waiting{fuel, Clock::now() + std::chrono::minutes(2)};
    UE_LOGI("[FIRE] client: fuel puddle key='%ls' lit on the host; its decal is not indexed here yet -- waiting",
            key.c_str());
}

}  // namespace coop::fire_sync
