// coop/props/water_fill_sync.cpp -- see coop/props/water_fill_sync.h.

#include "coop/props/water_fill_sync.h"

#include "coop/element/object_scan_hub.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/props/prop_key_index.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/water_container.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::water_fill_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace WC = ue_wrap::water_container;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowFaucetTick, "water_fill_sync", L"faucet_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowShowerTick, "water_fill_sync", L"prop_shower_C", L"ReceiveTick", Declared);
constexpr int kTagFaucet = 0x57465431;  // 'WFT1'
constexpr int kTagShower = 0x57465432;  // 'WFT2'

constexpr uint64_t kPumpMs = 500;       // the host looks at its buckets twice a second
constexpr float    kSendEps = 0.05f;    // a level change under this is not a change
constexpr float    kRiseEps = 1e-4f;    // a tick's rise under this is no fill
constexpr float    kMaxHeight = 1000.f;
constexpr int      kMaxSaid = 24;

std::atomic<coop::net::Session*> g_session{nullptr};

bool IsClient(coop::net::Session* s) {
    return s && s->running() && s->connected() && s->role() == coop::net::Role::Client;
}
bool IsHost(coop::net::Session* s) { return s && s->running() && s->role() == coop::net::Role::Host; }

uint64_t g_suppressed = 0, g_published = 0, g_applied = 0, g_seeded = 0, g_dropped = 0, g_unknown = 0;
int g_suppressSaid = 0, g_publishSaid = 0, g_applySaid = 0;

// ---- the index of this world's buckets (the scan hub's pass) ------------------------------------------------
struct Ref { void* actor; int32_t idx; };
std::vector<Ref> g_buckets;       // GT-only
std::vector<Ref> g_scanFound;     // pass scratch
uint32_t g_indexGen = 0;
bool IndexCurrent() { return g_indexGen == ue_wrap::world_identity::Generation(); }

void HubPassBegin(void*, bool) { g_scanFound.clear(); }

void HubMatch(void*, void* obj) {
    const std::wstring nm = R::ToString(R::NameOf(obj));
    if (nm.rfind(L"Default__", 0) == 0) return;
    if (!R::IsLive(obj)) return;
    g_scanFound.push_back(Ref{obj, R::InternalIndexOf(obj)});
}

// What the host last sent for one bucket (or took as its baseline when first seen).
struct Rec { int32_t idx; float last; bool lastEmpty; };
std::unordered_map<void*, Rec> g_ledger;   // GT-only; host

size_t HubPassComplete(void*, bool isFull, uint32_t worldGen) {
    if (isFull) {
        g_buckets = g_scanFound;
    } else {
        for (const Ref& f : g_scanFound) {
            bool known = false;
            for (const Ref& b : g_buckets)
                if (b.actor == f.actor && b.idx == f.idx) { known = true; break; }
            if (!known) g_buckets.push_back(f);
        }
        for (size_t i = 0; i < g_buckets.size();) {
            if (R::IsLiveByIndex(g_buckets[i].actor, g_buckets[i].idx)) ++i;
            else g_buckets.erase(g_buckets.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    // A bucket gone from the index leaves the ledger, so a recycled slot is a new bucket with a new baseline.
    for (auto it = g_ledger.begin(); it != g_ledger.end();) {
        bool live = false;
        for (const Ref& b : g_buckets)
            if (b.actor == it->first && b.idx == it->second.idx) { live = true; break; }
        if (live) ++it;
        else it = g_ledger.erase(it);
    }
    g_indexGen = worldGen;
    g_scanFound.clear();
    return g_buckets.size();
}

void RegisterWithScanHub() {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
        "bucket", nullptr, &WC::EnsureResolved, &WC::IsBucket, &HubPassBegin, &HubMatch, &HubPassComplete,
        /*settleScans*/ 2});
}

// ---- world props only ---------------------------------------------------------------------------------------
// The bucket in a player's hand is the hotbar's hand item, not a world prop; it has no prop key a peer could find.
// A key that resolves to this very actor is a world bucket. A lookup the index cannot answer yet counts as one: the
// world is where a bucket under a tap is.
bool WorldBucketKey(void* actor, std::wstring& keyOut, bool hitOnly) {
    std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
    if (key.empty() || key == L"None" || key.size() > sizeof(coop::net::WireKey::data)) return false;
    void* found = nullptr;
    const auto r = coop::prop_key_index::Resolve(key, &found);
    if (r == coop::prop_key_index::Read::Miss) return false;
    if (r == coop::prop_key_index::Read::Hit && found != actor) return false;
    if (r == coop::prop_key_index::Read::Pending && hitOnly) return false;
    keyOut = std::move(key);
    return true;
}

// ---- client: a tap's tick may not fill a shared bucket ------------------------------------------------------
struct Snap { void* actor; int32_t idx; WC::Bucket was; };
std::vector<Snap> g_snap;   // GT-only; one tap tick's worth (the tick runs no watched body inside it)

sg::Verdict OnTapTickPre(const sg::Call&) {
    g_snap.clear();
    if (!IsClient(g_session.load(std::memory_order_acquire)) || !IndexCurrent()) return sg::Verdict::Run;
    for (const Ref& b : g_buckets) {
        WC::Bucket v;
        if (R::IsLiveByIndex(b.actor, b.idx) && WC::ReadBucket(b.actor, v)) g_snap.push_back(Snap{b.actor, b.idx, v});
    }
    return sg::Verdict::Run;
}

void OnTapTickPost(const sg::Call& c) {
    if (g_snap.empty()) return;
    for (const Snap& s : g_snap) {
        WC::Bucket now;
        if (!R::IsLiveByIndex(s.actor, s.idx) || !WC::ReadBucket(s.actor, now) || now.height <= s.was.height + kRiseEps)
            continue;
        std::wstring key;
        if (!WorldBucketKey(s.actor, key, /*hitOnly*/ false)) continue;   // a hand-held bucket fills where it is held
        WC::WriteBucket(s.actor, s.was.height, s.was.empty, /*snapInterp*/ false);
        WC::RepaintBucket(s.actor);   // the tick painted the raised level; paint the one that stands
        ++g_suppressed;
        if (g_suppressSaid < kMaxSaid) {
            ++g_suppressSaid;
            UE_LOGI("[WATER] client: a tap's tick raised bucket key='%ls' %.2f -> %.2f -- put back, the host's level "
                    "stands (#%llu)", key.c_str(), static_cast<double>(s.was.height), static_cast<double>(now.height),
                    static_cast<unsigned long long>(g_suppressed));
        }
    }
    g_snap.clear();
    (void)c;
}

bool g_faucetWatched = false;
bool g_showerWatched = false;
bool g_settled = false;

void Settle() {
    if (g_settled || !g_faucetWatched || !g_showerWatched) return;
    const bool faucetLive = sg::NameWatchLive(kRowFaucetTick, kTagFaucet);
    const bool showerLive = sg::NameWatchLive(kRowShowerTick, kTagShower);
    if (faucetLive && showerLive) {
        g_settled = true;
        UE_LOGI("[WATER] the faucet and shower tick watches are live");
    } else if (sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[WATER] tap tick watch dead (faucet live=%d, shower live=%d) -- that tap still fills a client's bucket "
                "by itself", faucetLive ? 1 : 0, showerLive ? 1 : 0);
    }
}

// ---- host: the changed buckets --------------------------------------------------------------------------------
bool FillPayload(void* actor, const WC::Bucket& v, coop::net::BucketFillStatePayload& p) {
    std::wstring key;
    if (!WorldBucketKey(actor, key, /*hitOnly*/ true)) return false;
    p = coop::net::BucketFillStatePayload{};
    coop::net::WireKeyFromString(key, p.key);
    p.height = v.height < 0.f ? 0.f : (v.height > kMaxHeight ? kMaxHeight : v.height);
    p.flags = v.empty ? coop::net::bucket_fill_flags::kEmpty : 0;
    return true;
}

uint64_t g_lastPumpMs = 0;

void PumpHost(coop::net::Session& s) {
    const uint64_t now = coop::net::NowMs();
    if (now - g_lastPumpMs < kPumpMs) return;
    g_lastPumpMs = now;
    if (!IndexCurrent() || !WC::EnsureResolved()) return;
    const bool anyone = s.connected() && s.AnyWorldReadyPeer();
    for (const Ref& b : g_buckets) {
        WC::Bucket v;
        if (!R::IsLiveByIndex(b.actor, b.idx) || !WC::ReadBucket(b.actor, v) || !std::isfinite(v.height)) continue;
        auto it = g_ledger.find(b.actor);
        if (it == g_ledger.end() || it->second.idx != b.idx) {
            // First seen: its level is the baseline (a join's snapshot and the bucket's own birth record carry it).
            g_ledger[b.actor] = Rec{b.idx, v.height, v.empty};
            continue;
        }
        Rec& r = it->second;
        if (std::fabs(v.height - r.last) < kSendEps && v.empty == r.lastEmpty) continue;
        if (!anyone) {   // nobody to tell: the snapshot at a joiner's ready edge is the other half
            r.last = v.height;
            r.lastEmpty = v.empty;
            continue;
        }
        coop::net::BucketFillStatePayload p{};
        if (!FillPayload(b.actor, v, p)) {   // a hand-held bucket: not a world prop, not this lane's
            r.last = v.height;
            r.lastEmpty = v.empty;
            continue;
        }
        if (!s.SendReliable(coop::net::ReliableKind::BucketFillState, &p, sizeof(p))) continue;   // try again next pump
        ++g_published;
        if (g_publishSaid < kMaxSaid) {
            ++g_publishSaid;
            UE_LOGI("[WATER] host: bucket key='%ls' %.2f -> %.2f%s sent (#%llu)",
                    coop::net::StringFromWireKey(p.key).c_str(), static_cast<double>(r.last),
                    static_cast<double>(v.height), v.empty ? " (empty)" : "", static_cast<unsigned long long>(g_published));
        }
        r.last = v.height;
        r.lastEmpty = v.empty;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!session) return;
    RegisterWithScanHub();
    if (!g_faucetWatched) g_faucetWatched = sg::WatchName(kRowFaucetTick, kTagFaucet, &OnTapTickPre, &OnTapTickPost);
    if (!g_showerWatched) g_showerWatched = sg::WatchName(kRowShowerTick, kTagShower, &OnTapTickPre, &OnTapTickPost);
}

void Tick(coop::net::Session& session) {
    if (!g_settled) {
        sg::ResolvePendingNames();
        Settle();
    }
    if (IsHost(&session)) PumpHost(session);
}

void OnPeerWorldReady(int slot) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!IsHost(s) || slot < 1 || slot >= static_cast<int>(coop::net::kMaxPeers) || !IndexCurrent()) return;
    int sent = 0;
    for (const Ref& b : g_buckets) {
        WC::Bucket v;
        if (!R::IsLiveByIndex(b.actor, b.idx) || !WC::ReadBucket(b.actor, v) || !std::isfinite(v.height) ||
            v.height < kSendEps)
            continue;
        coop::net::BucketFillStatePayload p{};
        if (!FillPayload(b.actor, v, p)) continue;
        if (s->SendReliableToSlot(slot, coop::net::ReliableKind::BucketFillState, &p, sizeof(p))) ++sent;
    }
    g_seeded += static_cast<uint64_t>(sent);
    UE_LOGI("[WATER] host: slot %d's world is up -- sent %d bucket level(s) (of %zu indexed)", slot, sent, g_buckets.size());
}

void OnState(const coop::net::BucketFillStatePayload& p) {
    if (!IsClient(g_session.load(std::memory_order_acquire))) return;
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* actor = coop::prop_key_index::Find(key);
    WC::Bucket was;
    if (!actor || !WC::EnsureResolved() || !WC::IsBucket(actor) || !WC::ReadBucket(actor, was)) {
        ++g_unknown;   // not streamed in here yet: the bucket's own birth record carries its level
        return;
    }
    const bool empty = (p.flags & coop::net::bucket_fill_flags::kEmpty) != 0;
    // A bucket that does not simulate physics here does not tick, so nothing would ease `height_interp` to it.
    WC::WriteBucket(actor, p.height, empty, /*snapInterp*/ !WC::BucketSimulating(actor));
    WC::RepaintBucket(actor);
    ++g_applied;
    if (g_applySaid < kMaxSaid) {
        ++g_applySaid;
        UE_LOGI("[WATER] client: bucket key='%ls' %.2f -> %.2f%s (#%llu)", key.c_str(), static_cast<double>(was.height),
                static_cast<double>(p.height), empty ? " (empty)" : "", static_cast<unsigned long long>(g_applied));
    }
}

bool WellFormed(const coop::net::BucketFillStatePayload& p) {
    if (p.key.len == 0 || p.key.len > sizeof(p.key.data)) return false;
    if (p.reserved[0] || p.reserved[1] || p.reserved[2] || (p.flags & ~coop::net::bucket_fill_flags::kKnown) != 0)
        return false;
    return std::isfinite(p.height) && p.height >= 0.f && p.height <= kMaxHeight;
}

void CountDropped() { ++g_dropped; }

void OnDisconnect() {
    if (g_suppressed || g_published || g_applied || g_seeded || g_dropped || g_unknown)
        UE_LOGI("[WATER] session end -- tap fills put back=%llu, levels published=%llu seeded=%llu applied=%llu "
                "unknown=%llu dropped=%llu", static_cast<unsigned long long>(g_suppressed),
                static_cast<unsigned long long>(g_published), static_cast<unsigned long long>(g_seeded),
                static_cast<unsigned long long>(g_applied), static_cast<unsigned long long>(g_unknown),
                static_cast<unsigned long long>(g_dropped));
    g_snap.clear();
    g_ledger.clear();
    g_lastPumpMs = 0;
    g_suppressed = g_published = g_applied = g_seeded = g_dropped = g_unknown = 0;
    g_suppressSaid = g_publishSaid = g_applySaid = 0;
}

}  // namespace coop::water_fill_sync
