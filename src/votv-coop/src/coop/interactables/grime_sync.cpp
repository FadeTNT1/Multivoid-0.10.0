// coop/interactables/grime_sync.cpp -- see coop/interactables/grime_sync.h; the surface grime
// dirt sync. A sibling of the window sync (the keyed monotone-min-wins scalar channel) keyed
// by the decal's class, type, grid cell and exact position instead of a key name: the save gives
// every decal it holds an identical transform on each peer, so its position is its cross-peer identity. The
// one engine difference from the window: the apply repaints through the decal's own
// apply-material verb, not a pure setter. A fall is seen at the verbs that make one, watched on
// the script gate before and after their bodies: grime_C::clean (a sponge stroke, the rain) and
// the wall fixer's repair of a crack (wallFixer_fix), the two verbs that lower `process`. A verb
// that takes it below zero destroys the decal inside its body; the engine's end of play reports
// that destroy with its reason (coop/element/death_seam), so a wipe to destruction and a
// sublevel's stream-out are told apart by what the engine says, never by where the camera
// stands. If a third keyed-float channel appears, generalise the window and grime pair into a
// shared adapter and channel, as the interactable sync did for its bool features.

#include "coop/interactables/grime_sync.h"

#include "coop/config/config.h"
#include "coop/element/death_seam.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"  // WireKeyFromString / StringFromWireKey / FnvKey (shared)
#include "coop/player/players_registry.h"  // coop::players::kMaxPeers

#include "ue_wrap/engine/engine.h"          // TryGetActorLocation (the grime's world position -> posKey)
#include "ue_wrap/devices/grime.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/engine/world_identity.h"     // gen-stamped index (dead-world guard)
#include "coop/element/object_scan_hub.h"      // the shared sliced scan pass

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::grime_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace G = ue_wrap::grime;
namespace E = ue_wrap::engine;
namespace sg = ue_wrap::script_gate;
namespace DS = coop::element::death_seam;

using coop::net::WireKeyFromString;
using coop::net::StringFromWireKey;
using coop::net::FnvKey;

// The process changes in wipe steps, and a step can be smaller than this epsilon, so a fall is
// measured against the floor and small ones add up (OnLowerPost); it also skips an idempotent apply.
constexpr float kProcessEps = 0.0005f;
// The position quantisation grid (cm). A decal's key has two parts: its CELL -- its class, its type and
// its position on this grid -- and its EXACT place, the bits of its position. A decal's saved position
// is bit-identical across peers (the same save, a static actor), so the whole key names one decal on
// every peer, and twins of one cell (the rig's save had 1117 decals on 1087 grid-and-type cells, the old
// key, which also left the class out) are told apart. A key that does not match exactly falls back to
// its cell when the cell holds one decal here, which keeps the grid's tolerance for a decal whose
// position differs in its last bits.
constexpr double kPosGrid = 2.0;
// "G" + 12 hex digits of the cell; then "." + 8 hex digits of the exact place: 22 of the wire key's 31.
constexpr size_t kCellKeyLen = 13;
std::wstring CellOf(const std::wstring& key) { return key.size() > kCellKeyLen ? key.substr(0, kCellKeyLen) : key; }
bool ProbeLog() {
    static const bool s_enabled = ::coop::config::ResolveFlag(::coop::config_registry::rows::grime_log);
    return s_enabled;
}

std::atomic<coop::net::Session*> g_session{nullptr};

// An indexed decal: the actor, its slot and the serial the slot held when a pass took it (the key cache's
// own), so a new object the allocator put at the address in the same slot is never read as the decal.
struct Ref { void* actor; int32_t idx; int32_t serial; };
bool Holds(const Ref& r) { return R::IsLiveByIndex(r.actor, r.idx) && R::SlotSerial(r.idx) == r.serial; }
std::mutex g_indexMutex;
std::unordered_map<std::wstring, Ref> g_byKey;
// Per cell, the full keys the index holds in it: a key that does not match exactly resolves to the
// cell's one decal (CellFallback). Kept with g_byKey, under its lock.
std::unordered_map<std::wstring, std::vector<std::wstring>> g_byCell;

void IndexAdd(const std::wstring& key, const Ref& ref) {
    const bool fresh = g_byKey.find(key) == g_byKey.end();
    g_byKey[key] = ref;
    if (fresh) g_byCell[CellOf(key)].push_back(key);
}

void IndexErase(std::unordered_map<std::wstring, Ref>::iterator it) {
    const auto cell = g_byCell.find(CellOf(it->first));
    if (cell != g_byCell.end()) {
        auto& v = cell->second;
        v.erase(std::remove(v.begin(), v.end(), it->first), v.end());
        if (v.empty()) g_byCell.erase(cell);
    }
    g_byKey.erase(it);
}

// This peer's own name for one decal: its location's float bits and its type. It tells a cell's twins
// apart and never travels, so a value is enough, with no string.
struct ExactKey {
    uint32_t x = 0, y = 0, z = 0;
    int32_t type = 0;
    bool operator==(const ExactKey& o) const { return x == o.x && y == o.y && z == o.z && type == o.type; }
};
struct ExactKeyHash {
    size_t operator()(const ExactKey& k) const {
        uint64_t h = 1469598103934665603ull;  // FNV-1a over the four words
        for (uint32_t w : {k.x, k.y, k.z, static_cast<uint32_t>(k.type)}) h = (h ^ w) * 1099511628211ull;
        // A word-wise multiply leaves the low bits a function of the words' low bits alone, and a round
        // coordinate's float has them all zero; the table picks its bucket from the low bits. murmur3's
        // 64-bit finalizer spreads every bit into them.
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ull;
        h ^= h >> 33;
        return static_cast<size_t>(h);
    }
};

// A register of the running world: another world's is dropped the first time it is touched in this
// one, a value that arrives before the new world's first pass included. A peer leaving does not clear
// it. Game thread only.
template <class K, class V, class H = std::hash<K>>
struct WorldRegister {
    std::unordered_map<K, V, H> map;
    uint32_t gen = 0;
    std::unordered_map<K, V, H>& Get() {
        const uint32_t now = ue_wrap::world_identity::Generation();
        if (now != gen) {
            map.clear();
            gen = now;
        }
        return map;
    }
};

// The floor: per key, the lowest process every peer knows the key's decals at in this world -- a fall
// that was sent, by this peer or another, a destroy (zero, sent as a wipe), or the host's snapshot as
// adopted. It is the lane's shared register: a decal the index takes (a stream-back, one the join
// brought, one that was not here when its value arrived) is lowered to it, every decal of the key's cell
// alike, and the host's snapshot sends it for every key its index does not hold. A value only this peer
// holds enters it only while no peer is connected, a solo host's falls, which the join snapshot sends.
WorldRegister<std::wstring, float> g_floor;
std::unordered_map<std::wstring, float>& Floor() { return g_floor.Get(); }
// Per cell, the lowest floor of any key in it: what a decal alone in its cell here takes when its own
// exact key has no floor (a peer named it by bits this peer's copy does not have).
WorldRegister<std::wstring, float> g_cellFloor;

// Per decal (ExactKey), for a key with no floor yet: the process the decal had when this peer's first
// fall on it began, what every peer holds for a decal nobody has moved in this world, so falls under the
// epsilon add up from it.
WorldRegister<ExactKey, float, ExactKeyHash> g_unmoved;

// Per decal (its exact place), the process it had when it streamed out, with its key: the stream-back
// comes in at the level's value, and the pass that indexes it restores exactly that decal from here. The
// host's snapshot also sends it for a key its index does not hold. Kept out of the floor, which would
// lower the decal's cell-mates to a value only this peer holds.
struct Parked {
    std::wstring key;
    float value;
};
WorldRegister<ExactKey, Parked, ExactKeyHash> g_parked;

size_t g_lastLogCount = SIZE_MAX;  // GT-only: dedup the rebuilt log
uint64_t g_lastLogHash = 0;        // GT-only

// The cross-peer identity of a static grime decal: its quantised world position and type.
// The same save gives the identical saved transform, so the identical key on both peers.
// Fits the wire key for any in-base coordinate. `exact` is this peer's own name for the one decal
// (ExactKey). False when the location read fails: a key is never made from the origin a failed read
// leaves.
bool KeysOf(void* grime, std::wstring& key, ExactKey& exact) {
    ue_wrap::FVector loc{};
    if (!E::TryGetActorLocation(grime, loc)) return false;
    int32_t type = 0; G::ReadType(grime, type);
    auto q = [](float v) -> long { return std::lround(static_cast<double>(v) / kPosGrid); };
    std::memcpy(&exact.x, &loc.X, sizeof(exact.x));
    std::memcpy(&exact.y, &loc.Y, sizeof(exact.y));
    std::memcpy(&exact.z, &loc.Z, sizeof(exact.z));
    exact.type = type;
    // The cell: FNV-1a over the class name (a blood decal and a dirt decal of one type in one cell are
    // two things), the grid cell and the type.
    uint64_t cell = 1469598103934665603ull;
    auto mix = [&cell](uint64_t w) {
        for (int i = 0; i < 8; ++i) { cell ^= (w >> (i * 8)) & 0xFF; cell *= 1099511628211ull; }
    };
    for (wchar_t c : R::ClassNameOf(grime)) mix(static_cast<uint64_t>(::towlower(c)));
    mix(static_cast<uint64_t>(static_cast<int64_t>(q(loc.X))));
    mix(static_cast<uint64_t>(static_cast<int64_t>(q(loc.Y))));
    mix(static_cast<uint64_t>(static_cast<int64_t>(q(loc.Z))));
    mix(static_cast<uint64_t>(static_cast<uint32_t>(type)));
    // The exact place: FNV-1a over the position's bits.
    uint32_t ex = 2166136261u;
    for (uint32_t w : {exact.x, exact.y, exact.z, static_cast<uint32_t>(type)})
        for (int i = 0; i < 4; ++i) { ex ^= (w >> (i * 8)) & 0xFF; ex *= 16777619u; }
    wchar_t buf[32];
    std::swprintf(buf, 32, L"G%012llX.%08X", static_cast<unsigned long long>(cell & 0xFFFFFFFFFFFFull),
                  static_cast<unsigned>(ex));
    key = buf;
    return true;
}

std::wstring PosKey(void* grime) {
    std::wstring key;
    ExactKey exact;
    return KeysOf(grime, key, exact) ? key : std::wstring();
}

// The world generation of the last completed hub pass; a stale-generation index is treated
// as empty on every read path (the dead-world guard).
uint32_t g_indexGen = 0;
bool IndexCurrent() { return g_indexGen == ue_wrap::world_identity::Generation(); }

// The decal `key` names here, and the key this peer holds it under (`localKey`): the exact key, else
// the one decal of its cell.
void* ResolveFast(const std::wstring& key, std::wstring* localKey = nullptr) {
    if (!IndexCurrent()) return nullptr;  // a stale-generation index is another world's actors
    std::lock_guard<std::mutex> lk(g_indexMutex);
    auto it = g_byKey.find(key);
    if (it != g_byKey.end() && Holds(it->second)) {
        if (localKey) *localKey = key;
        return it->second.actor;
    }
    const auto cell = g_byCell.find(CellOf(key));
    if (cell == g_byCell.end() || cell->second.size() != 1) return nullptr;
    it = g_byKey.find(cell->second.front());
    if (it == g_byKey.end() || !Holds(it->second)) return nullptr;
    if (localKey) *localKey = it->first;
    return it->second.actor;
}

// Whether `key`'s cell holds that one decal alone here (its cell floor may stand for its own).
bool AloneInCell(const std::wstring& key) {
    std::lock_guard<std::mutex> lk(g_indexMutex);
    const auto cell = g_byCell.find(CellOf(key));
    return cell != g_byCell.end() && cell->second.size() == 1 && cell->second.front() == key;
}

void LowerCell(const std::wstring& key, float value) {
    auto& cf = g_cellFloor.Get();
    const std::wstring cell = CellOf(key);
    const auto it = cf.find(cell);
    if (it == cf.end() || value < it->second) cf[cell] = value;
}

// Lowers the floor of `key` to `value`; true when it moved.
bool Lower(const std::wstring& key, float value) {
    auto& floor = Floor();
    const auto it = floor.find(key);
    if (it != floor.end() && value >= it->second - kProcessEps) return false;
    floor[key] = value;
    LowerCell(key, value);
    return true;
}

// The floor a decal held under `key` here is lowered to: its own, else, alone in its cell, the cell's.
bool FloorFor(const std::wstring& key, float& out) {
    auto& floor = Floor();
    const auto it = floor.find(key);
    if (it != floor.end()) { out = it->second; return true; }
    auto& cf = g_cellFloor.Get();
    const auto c = cf.find(CellOf(key));
    if (c == cf.end() || !AloneInCell(key)) return false;
    out = c->second;
    return true;
}

// ---- the repaint that did not take ----
// A write whose repaint failed leaves the process written and the decal looking as before; the next
// apply of the same value would read the process, find it equal and skip. These decals are retried
// on their own, with a backoff, until the paint takes or the decal ends.
using SteadyClock = std::chrono::steady_clock;
struct VisualPending {
    int32_t idx;
    int32_t serial;
    std::wstring key;
    float target;
    int tries;
    SteadyClock::time_point next;
};
std::unordered_map<void*, VisualPending> g_visualPending;
constexpr size_t kMaxVisualPending = 256;
constexpr int kMaxVisualTries = 8;

// ---- counts for the once-a-minute summary ----
struct Counts {
    uint32_t sent = 0, applied = 0, captured = 0, pendingAdded = 0, pendingRetried = 0, pendingGaveUp = 0;
    uint32_t reconcileLowered = 0, reconcileSent = 0, cellFallback = 0;
};
Counts g_minute;
SteadyClock::time_point g_minuteStart{};

// Write `target` into a live decal and repaint it, unless it already reads that -- or, `lowerOnly`, unless
// it already reads that or less: a fall lowers a decal and never raises one this peer cleaned further.
// The apply runs no verb, so it is never heard back as a fall.
void ApplyResolved(void* actor, const std::wstring& key, float target, const char* why, bool lowerOnly) {
    float cur = 0.f;
    if (!G::ReadProcess(actor, cur)) return;
    // A decal whose last repaint failed reads the value it was meant to show: equality is not done.
    const bool pending = g_visualPending.count(actor) != 0;
    if (!pending && std::fabs(target - cur) < kProcessEps) return;
    if (lowerOnly && cur < target - kProcessEps) return;
    const bool ok = G::WriteProcessAndApply(actor, target);
    ++g_minute.applied;
    if (ok) {
        g_visualPending.erase(actor);
    } else if (g_visualPending.size() < kMaxVisualPending) {
        const int32_t idx = R::InternalIndexOf(actor);
        auto& vp = g_visualPending[actor];
        if (vp.key.empty()) ++g_minute.pendingAdded;
        vp = VisualPending{idx, R::SlotSerial(idx), key, target, vp.tries,
                           SteadyClock::now() + std::chrono::milliseconds(500)};
    }
    if (ProbeLog() || !ok)
        UE_LOGI("grime: applied process=%.3f (was %.3f, %s) ok=%d key='%ls'%s", target, cur, why, ok ? 1 : 0,
                key.c_str(), ok ? "" : " -- visual pending, retried");
}

void RetryVisualPending() {
    if (g_visualPending.empty()) return;
    const auto now = SteadyClock::now();
    for (auto it = g_visualPending.begin(); it != g_visualPending.end(); ) {
        VisualPending& vp = it->second;
        if (!R::IsLiveByIndex(it->first, vp.idx) || R::SlotSerial(vp.idx) != vp.serial) {
            it = g_visualPending.erase(it);
            continue;
        }
        if (now < vp.next) { ++it; continue; }
        ++vp.tries;
        if (G::WriteProcessAndApply(it->first, vp.target)) {
            ++g_minute.pendingRetried;
            UE_LOGI("[GRIME] visual pending retried key='%ls' process=%.3f -- painted after %d retr%s", vp.key.c_str(),
                    vp.target, vp.tries, vp.tries == 1 ? "y" : "ies");
            it = g_visualPending.erase(it);
            continue;
        }
        if (vp.tries >= kMaxVisualTries) {
            ++g_minute.pendingGaveUp;
            UE_LOGW("[GRIME] visual pending gave up key='%ls' process=%.3f after %d retries -- the decal shows its old "
                    "dirt until a later apply", vp.key.c_str(), vp.target, vp.tries);
            it = g_visualPending.erase(it);
            continue;
        }
        vp.next = now + std::chrono::milliseconds(500 << (vp.tries < 5 ? vp.tries : 5));
        ++it;
    }
}

// The shared-scan hub consumer: the per-module walk is gone, and the hub's shared sliced pass
// drives these callbacks. Two settle scans rather than fifteen: grime churns whenever the
// player washes (decal deaths re-arm the demand), so fifteen would pin a cleaning session in
// full-demand cadence; new decals append and the tail passes catch them, and recycled-slot
// arrivals heal within the hub's backstop. The key cache: a decal is static, so its key never
// changes, yet computing it dispatches a location UFunction, and doing that for a thousand
// unchanged decals on every rebuild was a recurring frame hitch; only a new decal computes
// it, and a full pass rebuilds the cache from the live set. An entry carries the slot and its
// engine serial (reflection::AllocateSlotSerial, the weak-pointer rule), so a new decal the
// allocator put at a dead one's address in the same slot never inherits the dead one's key.
struct CachedKey {
    std::wstring key;
    ExactKey     exact;  // KeysOf's own name for the one decal
    int32_t      idx;
    int32_t      serial;
};
std::unordered_map<void*, CachedKey> g_posKeyByActor;   // actor -> cached keys (GT-only)
std::unordered_map<void*, CachedKey> g_scanNextCache;   // pass scratch: full-pass cache rebuild
struct Found {
    std::wstring key;
    ExactKey     exact;
    Ref          ref;
};
std::vector<Found> g_scanFound;  // pass scratch (GT-only)
bool g_scanIsFull = false;                                  // pass context

void HubPassBegin(void*, bool isFull) {
    g_scanFound.clear();
    g_scanNextCache.clear();
    g_scanIsFull = isFull;
}

void HubMatch(void*, void* obj) {
    if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;  // skip the CDO (alloc-free)
    if (!R::IsLive(obj)) return;
    const int32_t idx = R::InternalIndexOf(obj);
    auto cit = g_posKeyByActor.find(obj);
    CachedKey ck;
    if (cit != g_posKeyByActor.end() && cit->second.idx == idx &&
        R::SlotSerial(idx) == cit->second.serial) {
        ck = cit->second;
    } else {
        if (!KeysOf(obj, ck.key, ck.exact)) return;  // no location read, so no identity to index it under
        ck.idx = idx;
        ck.serial = R::AllocateSlotSerial(idx);
    }
    if (g_scanIsFull) g_scanNextCache.insert_or_assign(obj, ck);   // full pass rebuilds the cache
    else              g_posKeyByActor.insert_or_assign(obj, ck);   // tail pass: cache the NEW actor's key
    const int32_t serial = ck.serial;
    g_scanFound.push_back(Found{std::move(ck.key), ck.exact, Ref{obj, idx, serial}});
}

size_t HubPassComplete(void*, bool isFull, uint32_t worldGen) {
    const size_t added = g_scanFound.size();
    if (isFull) g_posKeyByActor.swap(g_scanNextCache);  // keep live actors' cached keys, drop dead ones
    uint64_t posHash = 0;
    size_t   total;
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        if (isFull) { g_byKey.clear(); g_byCell.clear(); }   // full pass: rebuild from scratch
        for (auto& f : g_scanFound) IndexAdd(f.key, f.ref);
        if (!isFull) {                                         // tail pass: prune dead entries (cheap, O(index))
            for (auto it = g_byKey.begin(); it != g_byKey.end(); ) {
                if (Holds(it->second)) { ++it; continue; }
                g_posKeyByActor.erase(it->second.actor);
                auto dead = it++;
                IndexErase(dead);
            }
        }
        for (auto& kv : g_byKey) posHash ^= FnvKey(kv.first);  // recompute over the index (cheap, O(index))
        total = g_byKey.size();
        g_indexGen = worldGen;
    }
    // Every decal this pass found is lowered to its floor: one that streamed back in, one a join brought
    // after its value arrived, one this peer had not indexed when a peer's fall came. A pass spans frames,
    // so a decal matched early may have ended since: only one still live is read.
    size_t lowered = 0;
    if (!Floor().empty()) {
        for (const auto& f : g_scanFound) {
            if (!Holds(f.ref)) continue;
            float fl = 0.f;
            if (!FloorFor(f.key, fl)) continue;
            float cur = 0.f;
            if (!G::ReadProcess(f.ref.actor, cur) || cur <= fl + kProcessEps) continue;
            ApplyResolved(f.ref.actor, f.key, fl, "floor at index", /*lowerOnly=*/true);
            ++lowered;
        }
    }
    // A decal that streamed back in takes the process it streamed out at, this decal alone.
    auto& parked = g_parked.Get();
    if (!parked.empty()) {
        for (const auto& f : g_scanFound) {
            const auto it = parked.find(f.exact);
            if (it == parked.end() || !Holds(f.ref)) continue;
            ApplyResolved(f.ref.actor, f.key, it->second.value, "its stream-out", /*lowerOnly=*/true);
            parked.erase(it);
            ++lowered;
        }
    }
    if (total != g_lastLogCount || posHash != g_lastLogHash || lowered > 0) {
        g_lastLogCount = total;
        g_lastLogHash = posHash;
        UE_LOGI("grime: index rebuilt -- %zu live grime decal(s), posHash=0x%016llX (%s pass, +%zu new, %zu lowered "
                "to their floor) (compare host vs client for cross-peer position stability)",
                total, static_cast<unsigned long long>(posHash), isFull ? "full" : "tail", added, lowered);
    }
    if (ProbeLog())
        for (auto& f : g_scanFound)
            UE_LOGI("grime[probe]: key='%ls' idx=%d actor=%p", f.key.c_str(), f.ref.idx, f.ref.actor);
    g_scanFound.clear();
    g_scanNextCache.clear();
    return total;
}

void RegisterWithScanHub() {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
        "grime", nullptr, &G::EnsureResolved, &G::IsGrime,
        &HubPassBegin, &HubMatch, &HubPassComplete, /*settleScans*/ 2});
}

UE_FACT_FN(kRowClean, "grime_sync", nullptr, L"clean", AnyClass);
UE_FACT_FN(kRowWallFixerFix, "grime_sync", nullptr, L"wallFixer_fix", AnyClass);
constexpr int kTagGrimeClean = 0x4752494D;  // 'GRIM'
constexpr int kTagGrimeFix   = 0x47524958;  // 'GRIX'
bool g_cleanWatched = false;
bool g_fixWatched = false;
bool g_endSubscribed = false;

// The decal a verb is running on and its process before the body. A verb never nests on one decal.
void* g_lowering = nullptr;
float g_before = 0.f;

// A decal's identity, taken before a lowering verb runs: the verb can destroy the decal inside its
// body, and a decal no index pass has reached yet has no cached key, so its fall -- or its wipe, at
// its end of play -- would go out under nothing. Per actor, with its slot and serial; the end of play
// takes its entry. Bounded; another world's entries are dropped.
WorldRegister<void*, CachedKey> g_captured;
constexpr size_t kMaxCaptured = 512;

// The decal's cross-peer key, from the index's cache: only for a decal the current world's index holds
// under the slot and serial it still carries. A decal no pass has reached yet has none, and its fall is
// not sent; the floor of the peer it reaches next is the other side of that.
const CachedKey* CachedOf(void* actor, int32_t idx, int32_t serial) {
    if (IndexCurrent()) {
        const auto it = g_posKeyByActor.find(actor);
        if (it != g_posKeyByActor.end()) {
            const CachedKey& ck = it->second;
            if (ck.idx == idx && ck.serial != 0 && ck.serial == serial) return &ck;
        }
    }
    // A decal a lowering verb reached before any pass did.
    auto& cap = g_captured.Get();
    const auto c = cap.find(actor);
    if (c == cap.end() || c->second.idx != idx || c->second.serial == 0 || c->second.serial != serial) return nullptr;
    return &c->second;
}

// Take `actor`'s identity before a verb may end it: the index's key when it has one, else computed here
// (one location dispatch) and kept until the decal's fall or end has gone out.
void Capture(void* actor) {
    const int32_t idx = R::InternalIndexOf(actor);
    auto& cap = g_captured.Get();
    if (IndexCurrent()) {
        const auto it = g_posKeyByActor.find(actor);
        if (it != g_posKeyByActor.end() && it->second.idx == idx && it->second.serial == R::SlotSerial(idx) &&
            it->second.serial != 0) {
            if (cap.size() < kMaxCaptured) cap.insert_or_assign(actor, it->second);
            return;
        }
    }
    const auto c = cap.find(actor);
    if (c != cap.end() && c->second.idx == idx && c->second.serial == R::SlotSerial(idx) && c->second.serial != 0)
        return;
    CachedKey ck;
    if (!KeysOf(actor, ck.key, ck.exact)) return;
    ck.idx = idx;
    ck.serial = R::AllocateSlotSerial(idx);
    if (cap.size() >= kMaxCaptured) cap.clear();  // a burst no end of play drained: start over
    cap.insert_or_assign(actor, ck);
    ++g_minute.captured;
    if (ProbeLog()) UE_LOGI("grime: captured key='%ls' for a decal no pass has indexed yet", ck.key.c_str());
}

std::wstring KeyOf(void* actor, int32_t idx, int32_t serial) {
    const CachedKey* ck = CachedOf(actor, idx, serial);
    return ck ? ck->key : std::wstring();
}

void Send(const std::wstring& key, float value) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    coop::net::KeyedScalarPayload p{};
    WireKeyFromString(key, p.key);
    p.value = value;
    p.adopt = 0;  // a live fall: receivers apply the minimum
    if (!s->SendReliable(coop::net::ReliableKind::GrimeState, &p, sizeof(p))) {
        UE_LOGW("grime: SendReliable failed key='%ls'", key.c_str());
        return;
    }
    ++g_minute.sent;
    if (ProbeLog()) UE_LOGI("grime: sent process=%.3f key='%ls'", value, key.c_str());
}

sg::Verdict OnLowerPre(const sg::Call& call) {
    g_lowering = nullptr;
    if (call.object && G::IsGrime(call.object) && G::ReadProcess(call.object, g_before)) {
        g_lowering = call.object;
        Capture(call.object);
    }
    return sg::Verdict::Run;
}

// After the body: the process it left, sent when it fell. A verb that took it below zero destroyed the
// decal inside the body; its end of play sends the wipe instead (OnGrimeEnd).
void OnLowerPost(const sg::Call& call) {
    if (!g_lowering || call.object != g_lowering) return;
    g_lowering = nullptr;
    const int32_t idx = R::InternalIndexOf(call.object);
    if (!R::IsLiveByIndex(call.object, idx)) return;
    float cur = 0.f;
    if (!G::ReadProcess(call.object, cur) || cur >= g_before) return;  // an uncleanable decal (a crack)
    const CachedKey* ck = CachedOf(call.object, idx, R::SlotSerial(idx));
    if (!ck) {
        static int sSaid = 0;
        if (sSaid < 4) {
            ++sSaid;
            UE_LOGW("grime: a fall to %.3f had no identity (location unread) -- not sent", cur);
        }
        return;
    }
    const std::wstring key = ck->key;
    // A visual retry of this decal is overtaken by its own verb's paint.
    g_visualPending.erase(call.object);
    // Measured against the floor, the lowest value every peer knows for the key, or, before the key has
    // one, the process this decal had when this peer's first fall on it began: falls smaller than the
    // epsilon add up and go out once they pass it.
    auto& floor = Floor();
    const auto it = floor.find(key);
    auto& unmoved = g_unmoved.Get();
    const float from = it != floor.end() ? it->second : unmoved.emplace(ck->exact, g_before).first->second;
    if (cur >= from - kProcessEps) return;
    unmoved.erase(ck->exact);
    Lower(key, cur);
    Send(key, cur);
}

// A decal's end of play. A stream-out leaves the index and parks its process under its exact place,
// for its stream-back; a destroy -- a clean or a repair past zero, leaves raked or expired, fuel burnt
// out -- ends the decal on this peer, sent as a wipe to zero and kept as the floor for a joiner's
// snapshot.
void OnGrimeEnd(const DS::ActorEnd& end) {
    const CachedKey* ck = CachedOf(end.actor, end.actorIndex, end.actorSerial);
    if (!ck) return;
    const std::wstring key = ck->key;
    const ExactKey exact = ck->exact;
    // The actor ended before this drain; its memory is read only while its slot still holds it under the
    // serial it ended with, which a purge in between resets.
    float at = 0.f;
    const bool held = R::ObjectAt(end.actorIndex) == end.actor && R::SlotSerial(end.actorIndex) == end.actorSerial;
    const bool read = end.streamedOut && held && G::ReadProcess(end.actor, at);
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        const auto it = g_byKey.find(key);
        if (it != g_byKey.end() && it->second.actor == end.actor) g_byKey.erase(it);
    }
    g_posKeyByActor.erase(end.actor);
    g_captured.Get().erase(end.actor);
    g_visualPending.erase(end.actor);
    if (end.streamedOut) {
        if (read) g_parked.Get().insert_or_assign(exact, Parked{key, at});
        return;
    }
    if (!Lower(key, 0.f)) return;  // already wiped to zero here: said once
    Send(key, 0.f);
}

// ---- the bounded reconcile ----
// Once a second, a slice of the index is checked against the floor: a decal still showing more dirt than
// every peer knows the key at is lowered to it (a fall that arrived while the decal was not here, a twin
// a cell fall missed), and a decal this peer holds cleaner than the floor sends its value (a fall that
// went out under no identity). Sixty-four decals a second walks a thousand-decal base in about sixteen.
constexpr size_t kReconcilePerSecond = 64;
std::vector<std::pair<std::wstring, Ref>> g_reconcileList;
size_t g_reconcileCursor = 0;
SteadyClock::time_point g_nextReconcile{};

void ReconcileTick() {
    const auto now = SteadyClock::now();
    if (now < g_nextReconcile || !IndexCurrent()) return;
    g_nextReconcile = now + std::chrono::seconds(1);
    if (g_reconcileCursor >= g_reconcileList.size()) {
        g_reconcileCursor = 0;
        g_reconcileList.clear();
        std::lock_guard<std::mutex> lk(g_indexMutex);
        g_reconcileList.reserve(g_byKey.size());
        for (const auto& kv : g_byKey) g_reconcileList.emplace_back(kv.first, kv.second);
    }
    if (Floor().empty()) { g_reconcileCursor = g_reconcileList.size(); return; }
    const size_t end = std::min(g_reconcileList.size(), g_reconcileCursor + kReconcilePerSecond);
    for (; g_reconcileCursor < end; ++g_reconcileCursor) {
        const auto& [key, ref] = g_reconcileList[g_reconcileCursor];
        if (!Holds(ref)) continue;
        float fl = 0.f, cur = 0.f;
        if (!FloorFor(key, fl) || !G::ReadProcess(ref.actor, cur)) continue;
        if (cur > fl + kProcessEps) {
            ApplyResolved(ref.actor, key, fl, "reconcile", /*lowerOnly=*/true);
            ++g_minute.reconcileLowered;
        } else if (cur < fl - kProcessEps && cur >= 0.f) {
            Lower(key, cur);
            Send(key, cur);
            ++g_minute.reconcileSent;
        }
    }
}

void SummaryTick() {
    const auto now = SteadyClock::now();
    if (g_minuteStart == SteadyClock::time_point{}) g_minuteStart = now;
    if (now - g_minuteStart < std::chrono::minutes(1)) return;
    g_minuteStart = now;
    const Counts& c = g_minute;
    if (c.sent || c.applied || c.captured || c.pendingAdded || c.pendingRetried || c.pendingGaveUp ||
        c.reconcileLowered || c.reconcileSent || c.cellFallback)
        UE_LOGI("[GRIME] last minute: sent=%u applied=%u captured-before-index=%u visual pending added=%u "
                "retried ok=%u gave up=%u (open %zu), reconcile lowered=%u resent=%u, cell fallbacks=%u",
                c.sent, c.applied, c.captured, c.pendingAdded, c.pendingRetried, c.pendingGaveUp,
                g_visualPending.size(), c.reconcileLowered, c.reconcileSent, c.cellFallback);
    g_minute = Counts{};
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWithScanHub();  // the hub builds the index on its own cadence
}

void OnReliable(const coop::net::KeyedScalarPayload& payload, uint8_t senderPeerSlot) {
    std::wstring key = StringFromWireKey(payload.key);
    if (key.empty()) { UE_LOGW("grime: OnReliable empty key -- dropping"); return; }
    // The trust boundary (the same as the window's): adopt (as is, able to re-dirty) is honoured
    // only from the host; a client edge is forced to a min-wins live fall.
    const bool adopt = (payload.adopt != 0) && (senderPeerSlot == 0);
    // The floor takes the value whether or not the decal is here to show it. A peer's fall is written only
    // to a decal reading above it, and a pass lowers a decal to its floor and never raises one, so the host's
    // snapshot raises only a decal this peer indexes when the value arrives.
    auto& floor = Floor();
    if (adopt) {
        floor[key] = payload.value;
        LowerCell(key, payload.value);
    } else if (!Lower(key, payload.value)) {
        return;  // not below what this peer already holds
    }
    std::wstring localKey;
    if (void* actor = ResolveFast(key, &localKey)) {
        if (localKey != key) {
            // The cell's one decal here, named by bits this copy does not have: it is that decal, and it
            // keeps the value under its own key too, so the floor and the reconcile find it.
            ++g_minute.cellFallback;
            if (adopt) floor[localKey] = payload.value;
            else Lower(localKey, payload.value);
        }
        ApplyResolved(actor, localKey, floor[key], adopt ? "adopted" : "a peer's fall", /*lowerOnly=*/!adopt);
    }
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (s->role() != coop::net::Role::Host) return;  // host-only snapshot
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    if (!IndexCurrent()) {
        UE_LOGW("grime: connect-snapshot for slot %d skipped -- the index belongs to another world", peerSlot);
        return;
    }
    // The hub keeps the index at most one pass (about two seconds) fresh, so no forced rebuild: a
    // decal minted in that window reaches the joiner by its next fall. The connect edge is
    // game-thread-serial with the rebuild, so holding the lock across the sends is safe.
    int sent = 0;
    int floors = 0;
    size_t total = 0;
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        total = g_byKey.size();
        auto& floor = Floor();
        for (auto& kv : g_byKey) {
            if (!Holds(kv.second)) continue;
            float process = 0.f;
            if (!G::ReadProcess(kv.second.actor, process)) continue;
            // The lower of the decal and its floor: a cell's twin the host cleaned lowers the key until the
            // next full pass lowers this one too.
            const auto fl = floor.find(kv.first);
            if (fl != floor.end() && fl->second < process) process = fl->second;
            coop::net::KeyedScalarPayload p{};
            WireKeyFromString(kv.first, p.key);
            p.value = process;
            p.adopt = 1;  // the connect snapshot: the joiner adopts the host's world as is
            s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::GrimeState, &p, sizeof(p));
            ++sent;
        }
        // And every key the index does not hold live, at the lowest of its floor and its parked decals: a
        // decal ended here -- zero -- or streamed out, which the joiner has from its own copy of the save and
        // must see as this world has it.
        std::unordered_map<std::wstring, float> away(floor.begin(), floor.end());
        for (const auto& kv : g_parked.Get()) {
            const auto ins = away.emplace(kv.second.key, kv.second.value);
            if (!ins.second && kv.second.value < ins.first->second) ins.first->second = kv.second.value;
        }
        for (const auto& kv : away) {
            const auto idx = g_byKey.find(kv.first);
            if (idx != g_byKey.end() && Holds(idx->second)) continue;
            coop::net::KeyedScalarPayload p{};
            WireKeyFromString(kv.first, p.key);
            p.value = kv.second;
            p.adopt = 1;
            s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::GrimeState, &p, sizeof(p));
            ++floors;
        }
    }
    UE_LOGI("grime: connect-snapshot -- sent %d live process(es) + %d floor(s) of decals not indexed to slot %d "
            "(of %zu indexed)", sent, floors, peerSlot, total);
}

void Tick() {
    if (!G::EnsureResolved()) return;
    RegisterWithScanHub();  // safety net for any order where Tick precedes Install
    // The two lowering verbs' watches and the decals' end of play, each asked again until it takes. A
    // name watch matches the verb on any class; the pre half keeps only a grime decal.
    if (!g_cleanWatched) g_cleanWatched = sg::WatchName(kRowClean, kTagGrimeClean, &OnLowerPre, &OnLowerPost);
    if (!g_fixWatched) g_fixWatched = sg::WatchName(kRowWallFixerFix, kTagGrimeFix, &OnLowerPre, &OnLowerPost);
    if (!g_endSubscribed && DS::Install()) g_endSubscribed = DS::SubscribeClass(L"grime_C", &OnGrimeEnd);
    sg::ResolvePendingNames();
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) {
        g_visualPending.clear();
        return;
    }
    RetryVisualPending();
    ReconcileTick();
    SummaryTick();
}

std::wstring DebugPosKeyForActor(void* actor) {
    // The dev drill's parity probe: the real quantiser, cache-independent.
    return actor ? PosKey(actor) : std::wstring();
}

}  // namespace coop::grime_sync
