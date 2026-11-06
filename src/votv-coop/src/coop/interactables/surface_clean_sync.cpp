// coop/interactables/surface_clean_sync.cpp -- see coop/interactables/surface_clean_sync.h. The sink's, the
// shower's and the toilet's dirt scalar, the base window's mirrored (coop/interactables/window_sync.cpp) with the direction reversed:
// a wipe RAISES `clean` here, so a receiver applies the maximum.

#include "coop/interactables/surface_clean_sync.h"

#include "coop/element/object_scan_hub.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/surface_clean.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::surface_clean_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace SC = ue_wrap::surface_clean;
namespace sg = ue_wrap::script_gate;

using coop::net::StringFromWireKey;
using coop::net::WireKeyFromString;

constexpr auto kRetryThrottle = std::chrono::seconds(2);
constexpr auto kPendingTTL = std::chrono::seconds(25);
// `clean` rises in wipe steps (strength * a small constant per cleanSponge), and a step can be smaller than this
// epsilon, so a rise is measured against the value last sent and small ones add up (see Sent). It also skips an
// idempotent apply.
constexpr float kCleanEps = 0.0005f;
constexpr int   kMaxSaid = 24;

std::atomic<coop::net::Session*> g_session{nullptr};

struct Ref { void* actor; int32_t idx; };
std::mutex g_indexMutex;
std::unordered_map<std::wstring, Ref> g_byKey;

struct Pending { float clean; bool adopt; uint8_t fromSlot; std::chrono::steady_clock::time_point deadline; };
std::unordered_map<std::wstring, Pending> g_pending;  // GT-only: applies waiting for a surface to stream in

std::chrono::steady_clock::time_point g_lastRetry{};
size_t g_lastLogCount = SIZE_MAX;
uint32_t g_indexGen = 0;
bool IndexCurrent() { return g_indexGen == ue_wrap::world_identity::Generation(); }

int g_sentSaid = 0, g_appliedSaid = 0;

void* ResolveFast(const std::wstring& key) {
    if (!IndexCurrent()) return nullptr;  // a stale-generation index is another world's actors
    std::lock_guard<std::mutex> lk(g_indexMutex);
    auto it = g_byKey.find(key);
    if (it != g_byKey.end() && R::IsLiveByIndex(it->second.actor, it->second.idx)) return it->second.actor;
    return nullptr;
}

// ---- the shared-scan hub consumer --------------------------------------------------------------------------
std::vector<std::pair<std::wstring, Ref>> g_scanFound;  // pass scratch (GT-only)

void HubPassBegin(void*, bool) { g_scanFound.clear(); }

void HubMatch(void*, void* obj) {
    const std::wstring nm = R::ToString(R::NameOf(obj));
    if (nm.rfind(L"Default__", 0) == 0) return;
    if (!R::IsLive(obj)) return;
    std::wstring key = SC::GetKeyString(obj);
    if (key.empty() || key == L"None") return;
    g_scanFound.emplace_back(std::move(key), Ref{obj, R::InternalIndexOf(obj)});
}

size_t HubPassComplete(void*, bool isFull, uint32_t worldGen) {
    size_t total;
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        if (isFull) g_byKey.clear();
        for (auto& f : g_scanFound) g_byKey[f.first] = f.second;
        if (!isFull) {
            for (auto it = g_byKey.begin(); it != g_byKey.end();) {
                if (R::IsLiveByIndex(it->second.actor, it->second.idx)) ++it;
                else it = g_byKey.erase(it);
            }
        }
        total = g_byKey.size();
        g_indexGen = worldGen;
    }
    if (total != g_lastLogCount) {
        g_lastLogCount = total;
        UE_LOGI("[SURFACE] index now %zu live keyed sink/shower/toilet surface(s) (%s pass)", total, isFull ? "full" : "tail");
    }
    g_scanFound.clear();
    return total;
}

void RegisterWithScanHub() {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    coop::element::scan_hub::Consumer c{"surface_clean", nullptr, &SC::EnsureResolved, &SC::IsSurface, &HubPassBegin,
                                        &HubMatch, &HubPassComplete, /*settleScans*/ 2};
    c.ResolvedCount = &SC::ResolvedClassCount;
    coop::element::scan_hub::Register(c);
}

// Per surface, the value this peer last sent, seeded at the entry of its first wipe and raised by what it applies: a
// rise is measured against it, so rises smaller than the epsilon add up and go out once they pass it. Per world.
std::unordered_map<std::wstring, float> g_sent;  // GT-only
uint32_t g_sentGen = 0;
std::unordered_map<std::wstring, float>& Sent() {
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen != g_sentGen) {
        g_sent.clear();
        g_sentGen = gen;
    }
    return g_sent;
}

// Apply a remote clean value. An adopt takes it unchanged (the joiner adopts the host's world); a live wipe is
// MAX-wins, so a value below ours is ignored.
void ApplyResolved(void* actor, const std::wstring& key, float wireClean, bool adopt, unsigned fromSlot) {
    float cur = 0.f;
    if (!SC::ReadClean(actor, cur)) return;
    const float target = adopt ? wireClean : std::max(cur, wireClean);
    if (std::fabs(target - cur) < kCleanEps) return;
    const bool ok = SC::WriteCleanAndPaint(actor, target);
    if (ok) Sent()[key] = target;  // a local wipe after it is measured from what this peer now shows
    if (!ok || g_appliedSaid < kMaxSaid) {
        if (ok) ++g_appliedSaid;
        UE_LOGI("[SURFACE] applied %s clean=%.3f (was %.3f, wire=%.3f) ok=%d key='%ls' (from slot %u)",
                adopt ? "an adopt" : "a live wipe", static_cast<double>(target), static_cast<double>(cur),
                static_cast<double>(wireClean), ok ? 1 : 0, key.c_str(), fromSlot);
    }
}

// ---- the sender: the wipe verbs ----------------------------------------------------------------------------
UE_FACT_FN(kRowSinkSponge, "surface_clean_sync", L"sink_C", L"cleanSponge", Declared);
UE_FACT_FN(kRowShowerSponge, "surface_clean_sync", L"prop_shower_C", L"cleanSponge", Declared);
UE_FACT_FN(kRowToiletSponge, "surface_clean_sync", L"toilet_C", L"cleanSponge", Declared);
constexpr int kTagSink = 0x53574B31;    // 'SWK1'
constexpr int kTagShower = 0x53574B32;  // 'SWK2'
constexpr int kTagToilet = 0x53574B33;  // 'SWK3'
bool g_sinkWatched = false;
bool g_showerWatched = false;
bool g_toiletWatched = false;
bool g_settled = false;

// The surface a cleanSponge body runs on and its clean before the body. The body (a sink's, a shower's or a
// toilet's ubergraph) calls only native helpers and upd(), so no watched body runs inside it and one slot holds it.
void* g_wiping = nullptr;
float g_before = 0.f;

sg::Verdict OnWipePre(const sg::Call& call) {
    g_wiping = nullptr;
    if (call.object && SC::ReadClean(call.object, g_before)) g_wiping = call.object;
    return sg::Verdict::Run;
}

// After the body: the clean it left, sent when it rose. A session with nobody connected has no one to send to; the
// joiner's snapshot is the connect half.
void OnWipePost(const sg::Call& call) {
    if (!g_wiping || call.object != g_wiping) return;
    g_wiping = nullptr;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    float cur = 0.f;
    if (!SC::ReadClean(call.object, cur) || cur <= g_before) return;  // at 1 the body raises nothing
    const std::wstring key = SC::GetKeyString(call.object);
    if (key.empty() || key == L"None") return;
    auto& sent = Sent();
    const auto it = sent.find(key);
    const float base = it != sent.end() ? it->second : g_before;
    if (cur <= base + kCleanEps && cur < 1.0f) {  // under the epsilon from what was sent, short of spotless
        if (it == sent.end()) sent[key] = g_before;
        return;
    }
    sent[key] = cur;
    coop::net::KeyedScalarPayload p{};
    WireKeyFromString(key, p.key);
    p.value = cur;
    p.adopt = 0;  // a live wipe: receivers apply the maximum
    if (!s->SendReliable(coop::net::ReliableKind::SurfaceCleanState, &p, sizeof(p))) {
        UE_LOGW("[SURFACE] SendReliable failed key='%ls'", key.c_str());
    } else if (g_sentSaid < kMaxSaid) {
        ++g_sentSaid;
        UE_LOGI("[SURFACE] sent clean=%.3f key='%ls'", static_cast<double>(cur), key.c_str());
    }
}

void Settle() {
    if (g_settled || !g_sinkWatched || !g_showerWatched || !g_toiletWatched) return;
    const bool sinkLive = sg::NameWatchLive(kRowSinkSponge, kTagSink);
    const bool showerLive = sg::NameWatchLive(kRowShowerSponge, kTagShower);
    const bool toiletLive = sg::NameWatchLive(kRowToiletSponge, kTagToilet);
    if (sinkLive && showerLive && toiletLive) {
        g_settled = true;
        UE_LOGI("[SURFACE] the sink, shower and toilet cleanSponge watches are live");
    } else if (sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[SURFACE] cleanSponge watch dead (sink live=%d, shower live=%d, toilet live=%d) -- that surface's "
                "wipes stay on the peer that made them", sinkLive ? 1 : 0, showerLive ? 1 : 0, toiletLive ? 1 : 0);
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWithScanHub();
    if (!g_sinkWatched)
        g_sinkWatched = sg::WatchName(kRowSinkSponge, kTagSink, &OnWipePre, &OnWipePost);
    if (!g_showerWatched)
        g_showerWatched = sg::WatchName(kRowShowerSponge, kTagShower, &OnWipePre, &OnWipePost);
    if (!g_toiletWatched)
        g_toiletWatched = sg::WatchName(kRowToiletSponge, kTagToilet, &OnWipePre, &OnWipePost);
}

void OnReliable(const coop::net::KeyedScalarPayload& payload, uint8_t senderPeerSlot) {
    std::wstring key = StringFromWireKey(payload.key);
    if (key.empty()) { UE_LOGW("[SURFACE] OnReliable empty key -- dropping"); return; }
    if (!SC::EnsureResolved()) {
        UE_LOGW("[SURFACE] apply -- classes not resolved, dropping key='%ls'", key.c_str());
        return;
    }
    // Trust boundary: an unchanged adopt (which bypasses max-wins and so CAN lower a surface's clean) is host-only. A
    // client claiming one could otherwise dirty the world.
    const bool adopt = (payload.adopt != 0) && (senderPeerSlot == 0);
    if (void* actor = ResolveFast(key)) { ApplyResolved(actor, key, payload.value, adopt, senderPeerSlot); return; }
    // Not streamed in yet -- defer and retry on the throttled tick. An adopt takes the value unchanged; a live wipe
    // keeps the higher of the two, the same max-wins rule the direct apply uses.
    const auto deadline = std::chrono::steady_clock::now() + kPendingTTL;
    auto it = g_pending.find(key);
    if (it == g_pending.end()) {
        g_pending[key] = Pending{payload.value, adopt, senderPeerSlot, deadline};
    } else {
        if (adopt) { it->second.clean = payload.value; it->second.adopt = true; }
        else       { it->second.clean = std::max(it->second.clean, payload.value); }
        it->second.fromSlot = senderPeerSlot;
        it->second.deadline = deadline;
    }
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    std::vector<std::pair<std::wstring, Ref>> items;
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        items.reserve(g_byKey.size());
        for (auto& kv : g_byKey) items.emplace_back(kv.first, kv.second);
    }
    int sent = 0;
    for (auto& d : items) {
        if (!R::IsLiveByIndex(d.second.actor, d.second.idx)) continue;
        float clean = 0.f;
        if (!SC::ReadClean(d.second.actor, clean)) continue;
        coop::net::KeyedScalarPayload p{};
        WireKeyFromString(d.first, p.key);
        p.value = clean;
        p.adopt = 1;  // the joiner adopts the host's world unchanged
        if (s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::SurfaceCleanState, &p, sizeof(p))) ++sent;
    }
    UE_LOGI("[SURFACE] connect snapshot -- sent %d sink/shower clean value(s) to slot %d (of %zu indexed)", sent,
            peerSlot, items.size());
}

void Tick() {
    if (!SC::EnsureResolved()) return;
    RegisterWithScanHub();  // safety net for any order where Tick precedes Install
    if (!g_settled) {
        sg::ResolvePendingNames();
        Settle();
    }
    if (!IndexCurrent()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastRetry < kRetryThrottle) return;
    g_lastRetry = now;
    if (g_pending.empty()) return;
    int applied = 0, expired = 0, still = 0;
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        if (void* actor = ResolveFast(it->first)) {
            ApplyResolved(actor, it->first, it->second.clean, it->second.adopt, it->second.fromSlot);
            it = g_pending.erase(it);
            ++applied;
        } else if (now >= it->second.deadline) {
            it = g_pending.erase(it);
            ++expired;
        } else { ++it; ++still; }
    }
    if (applied || expired)
        UE_LOGI("[SURFACE] retry tick -- applied %d deferred, dropped %d expired, %d still pending", applied, expired,
                still);
}

void OnDisconnect() {
    g_pending.clear();
    g_wiping = nullptr;
    g_sent.clear();
    g_sentSaid = 0;
    g_appliedSaid = 0;
}

}  // namespace coop::surface_clean_sync
