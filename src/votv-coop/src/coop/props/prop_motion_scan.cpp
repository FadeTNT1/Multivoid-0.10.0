// coop/props/prop_motion_scan.cpp -- see coop/props/prop_motion_scan.h.

#include "coop/props/prop_motion_scan.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/active_drive.h"          // NowMs
#include "coop/props/prop_drive_host.h"       // Coast, IsDriven, InAHand, CoastingCount
#include "coop/props/prop_lifecycle.h"        // IsWireSuppressedPropClass
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"       // GetCameraLocation: the host's own view
#include "ue_wrap/engine/engine_physics.h"    // IsComponentSimulatingPhysics

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace coop::prop_motion_scan {

// The pure rules, checked where they are compiled.
static_assert(CoastSlots(0, 0) == kMaxNewCoastsPerTick, "an idle channel takes the per-tick cap");
static_assert(CoastSlots(kMaxCoasting, 0) == 0, "a full channel takes nothing");
static_assert(CoastSlots(kMaxCoasting - 1, 0) == 1, "the concurrent cap bounds the per-tick one");
static_assert(CoastSlots(0, kMaxNewCoastsPerTick) == 0, "a tick's cap, once spent, is spent");
static_assert(CoastSlots(kMaxCoasting + 5, 0) == 0, "a channel over the cap (other feeders) takes nothing");
static_assert(ShouldCoast(true, true, false, false), "a simulating prop that moved coasts");
static_assert(!ShouldCoast(false, true, false, false), "a prop that did not move does not");
static_assert(!ShouldCoast(true, false, false, false), "a kinematic prop is some other lane's");
static_assert(!ShouldCoast(true, true, true, false), "a driven prop is its row's");
static_assert(!ShouldCoast(true, true, false, true), "a held prop is its hand's");
static_assert(kMaxCoasting < 32, "stays below the host-birth feeder's cap (host_spawn_watcher)");

namespace {

namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
namespace R  = ue_wrap::reflection;

struct Entry {
    void*             actor = nullptr;
    int32_t           idx   = -1;
    ue_wrap::FVector  baseLoc{};
    ue_wrap::FRotator baseRot{};
    bool              hasBase  = false;
    bool              eligible = false;   // a keyed Aprop_C of a class the wire expresses; judged once per actor
};

std::unordered_map<uint32_t, Entry> g_entries;   // eid -> state, the host's own props
std::vector<uint32_t> g_near;                    // eids within kNearCm of a player at the last refresh
std::vector<uint32_t> g_far;
size_t   g_nearCursor = 0;
size_t   g_farCursor  = 0;
uint64_t g_nextRefreshMs = 0;
size_t   g_coastsOpened = 0;
size_t   g_nextSayAt = 1;   // the log line's next count: the first coast, then every 64
std::vector<coop::element::Registry::ActorIdPair> g_snap;   // kept for its capacity

// The places of every player: the host's view and each live puppet.
struct Players {
    ue_wrap::FVector at[coop::players::kMaxPeers + 1];
    int count = 0;
};

Players ReadPlayers() {
    Players p;
    p.at[p.count++] = E::GetCameraLocation();
    for (uint8_t s = 0; s < coop::players::kMaxPeers && p.count < coop::players::kMaxPeers + 1; ++s) {
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(s);
        void* body = rp ? rp->GetActor() : nullptr;
        ue_wrap::FVector loc{};
        if (body && R::IsLive(body) && E::TryGetActorLocation(body, loc)) p.at[p.count++] = loc;
    }
    return p;
}

bool NearAPlayer(const ue_wrap::FVector& loc, const Players& p) {
    constexpr float kNear2 = kNearCm * kNearCm;
    for (int i = 0; i < p.count; ++i) {
        const float dx = loc.X - p.at[i].X, dy = loc.Y - p.at[i].Y, dz = loc.Z - p.at[i].Z;
        if (dx * dx + dy * dy + dz * dz <= kNear2) return true;
    }
    return false;
}

// The candidate lists, rebuilt from the Registry: the host's own Prop elements (never a peer's wire
// mirror, whose motion is its owner's), split by distance to the nearest player. Baselines of the
// props still here are kept; a recycled eid on a new actor starts over.
void Refresh() {
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, g_snap);
    const Players players = ReadPlayers();
    std::unordered_map<uint32_t, Entry> next;
    next.reserve(g_snap.size());
    g_near.clear();
    g_far.clear();
    for (const auto& pr : g_snap) {
        if (pr.mirror || !pr.actor || pr.id == 0) continue;
        if (!R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;
        const uint32_t eid = static_cast<uint32_t>(pr.id);
        Entry e;
        auto old = g_entries.find(eid);
        if (old != g_entries.end() && old->second.actor == pr.actor && old->second.idx == pr.internalIdx) {
            e = old->second;
        } else {
            e.actor = pr.actor;
            e.idx   = pr.internalIdx;
            e.eligible = PR::IsDescendantOfProp(pr.actor) &&
                         !coop::prop_lifecycle::IsWireSuppressedPropClass(R::ClassNameOf(pr.actor));
        }
        if (!e.eligible) continue;
        ue_wrap::FVector loc{};
        const bool near = e.hasBase ? NearAPlayer(e.baseLoc, players)
                                    : (E::TryGetActorLocation(pr.actor, loc) && NearAPlayer(loc, players));
        (near ? g_near : g_far).push_back(eid);
        next.emplace(eid, e);
    }
    g_entries.swap(next);
    g_nearCursor = g_nearCursor < g_near.size() ? g_nearCursor : 0;
    g_farCursor  = g_farCursor  < g_far.size()  ? g_farCursor  : 0;
}

// One read of one candidate. Returns true when it opened a coast.
bool Check(uint32_t eid, bool mayCoast) {
    auto it = g_entries.find(eid);
    if (it == g_entries.end()) return false;
    Entry& e = it->second;
    if (!R::IsLiveByIndex(e.actor, e.idx)) {
        g_entries.erase(it);   // the next refresh drops its eid from the lists
        return false;
    }
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    if (!E::TryGetActorLocation(e.actor, loc) || !E::TryGetActorRotation(e.actor, rot)) return false;
    if (!e.hasBase) {
        e.baseLoc = loc;
        e.baseRot = rot;
        e.hasBase = true;
        return false;
    }
    const bool driven = coop::prop_drive_host::IsDriven(e.actor);
    const bool inHand = !driven && coop::prop_drive_host::InAHand(e.actor);
    const bool moved  = MovedPastScanEps(loc, rot, e.baseLoc, e.baseRot);
    if (driven || inHand || !moved) {
        // Another lane moves it, or nothing did: the baseline follows, so the prop a row just brought
        // to rest is not coasted again for the motion that row already streamed.
        if (moved) {
            e.baseLoc = loc;
            e.baseRot = rot;
        }
        return false;
    }
    void* mesh = PR::GetStaticMesh(e.actor);
    const bool simulating = mesh && E::IsComponentSimulatingPhysics(mesh);
    // A kinematic prop moved by some other lane (a hook's tie, an attach, a script) is that lane's;
    // its new place is the baseline.
    if (!ShouldCoast(moved, simulating, driven, inHand) || !mayCoast) {
        if (!simulating) {
            e.baseLoc = loc;
            e.baseRot = rot;
        }
        return false;   // over the coast budget: the next pass sees it moved still
    }
    coop::prop_drive_host::Coast(e.actor, "motion scan");
    e.baseLoc = loc;
    e.baseRot = rot;
    if (!coop::prop_drive_host::IsDriven(e.actor)) return false;   // the channel refused it
    ++g_coastsOpened;
    return true;
}

void Sweep(std::vector<uint32_t>& list, size_t& cursor, size_t reads, size_t& opened) {
    const size_t n = list.size();
    if (n == 0) return;
    if (reads > n) reads = n;
    for (size_t k = 0; k < reads; ++k) {
        if (cursor >= n) cursor = 0;
        const uint32_t eid = list[cursor++];
        const bool may = CoastSlots(coop::prop_drive_host::CoastingCount(), opened) > 0;
        if (Check(eid, may)) ++opened;
    }
}

}  // namespace

void Tick(coop::net::Session& /*s*/, bool isHost) {
    UE_ASSERT_GAME_THREAD("prop_motion_scan::Tick");
    if (!isHost) return;
    const uint64_t now = coop::active_drive::NowMs();
    if (now >= g_nextRefreshMs) {
        Refresh();
        g_nextRefreshMs = now + kRefreshMs;
    }
    size_t opened = 0;
    // The near set first: when the budget runs short, the props a player can see take it.
    Sweep(g_near, g_nearCursor, kNearChecksPerTick, opened);
    Sweep(g_far, g_farCursor, kFarChecksPerTick, opened);
    if (opened && g_coastsOpened >= g_nextSayAt) {
        g_nextSayAt = g_coastsOpened + 64;
        UE_LOGI("[PROP-SCAN] HOST %zu coast(s) opened this session for motion no verb owns (%zu near, %zu far "
                "candidates)", g_coastsOpened, g_near.size(), g_far.size());
    }
}

size_t CoastsOpened() {
    return g_coastsOpened;
}

void OnDisconnect() {
    g_entries.clear();
    g_near.clear();
    g_far.clear();
    g_snap.clear();
    g_nearCursor = g_farCursor = 0;
    g_nextRefreshMs = 0;
    g_coastsOpened = 0;
    g_nextSayAt = 1;
}

}  // namespace coop::prop_motion_scan
