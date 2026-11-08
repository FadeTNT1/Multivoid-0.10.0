// coop/world/event_count_upkeep.cpp -- see coop/world/event_count_upkeep.h.

#include "coop/world/event_count_upkeep.h"

#include "coop/net/session.h"
#include "coop/world/creature_event_relay.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/active_events.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::event_count_upkeep {
namespace {

namespace R  = ue_wrap::reflection;
namespace AE = ue_wrap::active_events;
namespace WI = ue_wrap::world_identity;

constexpr long long kPollMs = 1000;
constexpr int       kStablePolls = 3;        // an excess must stand this many polls before it is taken back: the
                                             // relay's own retraction of a dead mirror lands inside the first
constexpr long long kHeldLogMs = 30000;      // a count held this long is named, then again at this pace
constexpr size_t    kMaxTracked = 256;
constexpr size_t    kMaxVanished = 8;
constexpr int       kLineBudget = 40;        // begin/end lines per window
constexpr long long kBudgetWindowMs = 10000;

// A sender seen in the registry: its slot (recycled-slot-safe liveness), class and when it was first seen.
struct Tracked {
    int32_t     idx;
    std::string cls;
    long long   firstMs;
};
std::unordered_map<void*, Tracked> g_tracked;
std::vector<std::string> g_vanished;   // classes of senders destroyed while registered, until the count is reconciled
uint32_t  g_gen = 0;
long long g_lastPollMs = 0;
long long g_heldSinceMs = 0;           // when the count rose above 0; 0 while it is 0
long long g_lastHeldLogMs = 0;
int       g_excessPolls = 0;
long long g_budgetStartMs = 0;
int       g_budgetUsed = 0;
int       g_suppressed = 0;

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
    return s;
}

// The begin and end lines of a busy moment are capped; what was dropped is counted and said in the next window.
bool MayLog(long long now) {
    if (now - g_budgetStartMs >= kBudgetWindowMs) {
        if (g_suppressed > 0)
            UE_LOGI("event_active[client]: %d begin/end line(s) not logged (more than %d in %llds)", g_suppressed,
                    kLineBudget, kBudgetWindowMs / 1000);
        g_budgetStartMs = now;
        g_budgetUsed = 0;
        g_suppressed = 0;
    }
    if (g_budgetUsed < kLineBudget) { ++g_budgetUsed; return true; }
    ++g_suppressed;
    return false;
}

struct Registry {
    int32_t             count = 0;
    int                 live = 0;     // entries naming a live object, a duplicate counted each time
    int                 dead = 0;     // null or collected entries
    std::vector<void*>  liveObjs;
};

bool ReadRegistry(Registry& r) {
    AE::Senders arr{};
    if (!AE::EnsureResolved() || !AE::ReadCount(r.count) || !AE::ReadSenders(arr)) return false;
    if (arr.num < 0 || arr.num > 4096) return false;
    for (int32_t i = 0; i < arr.num; ++i) {
        void* obj = arr.data ? arr.data[i] : nullptr;
        if (obj && R::IsLive(obj)) { ++r.live; r.liveObjs.push_back(obj); }
        else ++r.dead;
    }
    return true;
}

// Seed the live senders, and settle the tracked ones that left the registry: a destroyed one is remembered by class
// (the next reconcile names it), one that merely left ended with its own false.
void Observe(const Registry& r, long long now) {
    std::unordered_set<void*> liveSet(r.liveObjs.begin(), r.liveObjs.end());
    for (void* obj : liveSet) {
        if (g_tracked.size() >= kMaxTracked) break;
        if (g_tracked.find(obj) == g_tracked.end())
            g_tracked.emplace(obj, Tracked{R::InternalIndexOf(obj), Narrow(R::ClassNameOf(obj)), now});
    }
    for (auto it = g_tracked.begin(); it != g_tracked.end();) {
        if (liveSet.count(it->first)) { ++it; continue; }
        if (!R::IsLiveByIndex(it->first, it->second.idx) && g_vanished.size() < kMaxVanished)
            g_vanished.push_back(it->second.cls);
        it = g_tracked.erase(it);
    }
}

std::string TrackedList(long long now) {
    std::string out;
    int shown = 0;
    for (const auto& kv : g_tracked) {
        if (shown++ >= 8) { out += "..."; break; }
        out += kv.second.cls + "(" + std::to_string((now - kv.second.firstMs) / 1000) + "s) ";
    }
    return out.empty() ? "none" : out;
}

void ResetWorld() {
    g_tracked.clear();
    g_vanished.clear();
    g_heldSinceMs = 0;
    g_lastHeldLogMs = 0;
    g_excessPolls = 0;
}

int CopiesOf(void* sender) {
    AE::Senders arr{};
    if (!AE::EnsureResolved() || !AE::ReadSenders(arr) || arr.num < 0 || arr.num > 4096) return 0;
    int n = 0;
    for (int32_t i = 0; i < arr.num; ++i)
        if (arr.data && arr.data[i] == sender) ++n;
    return n;
}

}  // namespace

Held Snapshot(void* sender) {
    Held h;
    if (!sender) return h;
    h.copies = CopiesOf(sender);
    if (h.copies > 0) h.className = R::ClassNameOf(sender);
    return h;
}

void ReleaseLeft(void* sender, const Held& before, const char* site) {
    if (before.copies <= 0) return;
    // The registry names the sender by pointer, which a destroyed object keeps until the collector nulls it.
    const int left = CopiesOf(sender);
    if (left <= 0) return;   // its own Blueprint took them back
    const int released = coop::creature_event_relay::ReleaseOnGamemode(left);
    g_tracked.erase(sender);
    UE_LOGW("event_active[client]: %s -- %ls was destroyed holding %d registration(s) its Blueprint did not take back; "
            "%d released", site, before.className.c_str(), left, released);
}

int ReclaimDead(const char* why) {
    Registry r;
    if (!ReadRegistry(r)) return 0;
    Observe(r, NowMs());
    const int excess = r.count - r.live;
    if (excess <= 0) return 0;
    const int released = coop::creature_event_relay::ReleaseOnGamemode(excess);
    int32_t now = 0;
    // The call could not be made (no library object yet): the count is set to what the live senders hold.
    if (AE::ReadCount(now) && now > r.live) AE::WriteCount(r.live);
    AE::ReadCount(now);
    std::string who;
    for (const std::string& c : g_vanished) who += c + " ";
    UE_LOGW("event_active[client]: activeEvents was %d with %d live sender(s) (%s) -- %d registration(s) of senders "
            "destroyed without their setEvent(false) taken back (%d by setEvent, vanished: %s); now %d",
            r.count, r.live, why, excess, released, who.empty() ? "none seen" : who.c_str(), now);
    g_vanished.clear();
    return excess;
}

void Tick(coop::net::Session* session) {
    if (!session || !session->connected() || session->role() != coop::net::Role::Client) return;
    const long long now = NowMs();
    if (now - g_lastPollMs < kPollMs) return;
    g_lastPollMs = now;
    const uint32_t gen = WI::Generation();
    if (gen != g_gen) { g_gen = gen; ResetWorld(); }
    Registry r;
    if (!ReadRegistry(r)) return;
    Observe(r, now);
    if (r.count <= 0) {
        g_heldSinceMs = 0;
        g_excessPolls = 0;
        g_vanished.clear();
        return;
    }
    if (g_heldSinceMs == 0) { g_heldSinceMs = now; g_lastHeldLogMs = now; }
    g_excessPolls = (r.count > r.live) ? g_excessPolls + 1 : 0;
    if (g_excessPolls >= kStablePolls) {
        g_excessPolls = 0;
        ReclaimDead("the excess stood for three polls");
        return;
    }
    if (now - g_heldSinceMs >= kHeldLogMs && now - g_lastHeldLogMs >= kHeldLogMs) {
        g_lastHeldLogMs = now;
        UE_LOGI("event_active[client]: activeEvents=%d held for %llds, %d live sender(s) [%s]", r.count,
                (now - g_heldSinceMs) / 1000, r.live, TrackedList(now).c_str());
    }
}

void OnClientSetEvent(void* sender, bool active) {
    if (!sender) return;
    const std::wstring wcls = R::ClassNameOf(sender);
    // The relay takes a stale registration off with the gamemode as the context; it logs that itself.
    if (wcls == L"mainGamemode_C") return;
    const long long now = NowMs();
    const std::string cls = Narrow(wcls);
    int32_t n = 0;
    AE::ReadCount(n);
    if (active) {
        if (g_tracked.size() < kMaxTracked && g_tracked.find(sender) == g_tracked.end())
            g_tracked.emplace(sender, Tracked{R::InternalIndexOf(sender), cls, now});
        if (MayLog(now)) UE_LOGI("event_active[client]: BEGIN class=%s n=%d", cls.c_str(), n);
        return;
    }
    const auto it = g_tracked.find(sender);
    const long long age = it == g_tracked.end() ? -1 : (now - it->second.firstMs) / 1000;
    if (it != g_tracked.end()) g_tracked.erase(it);
    if (!MayLog(now)) return;
    if (age < 0) UE_LOGI("event_active[client]: END class=%s n=%d (not seen to begin)", cls.c_str(), n);
    else UE_LOGI("event_active[client]: END class=%s n=%d elapsed=%llds", cls.c_str(), n, age);
}

void OnDisconnect() {
    ResetWorld();
    g_gen = 0;
    g_lastPollMs = 0;
}

}  // namespace coop::event_count_upkeep
