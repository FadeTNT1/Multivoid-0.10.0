// coop/props/client_shove.cpp -- see coop/props/client_shove.h.

#include "coop/props/client_shove.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/active_drive.h"          // NowMs
#include "coop/props/prop_drive_host.h"       // IsDriven, InAHand
#include "coop/props/prop_drive_stream.h"     // IsParked, LastEndAgeMs
#include "coop/props/prop_lifecycle.h"        // IsWireSuppressedPropClass
#include "coop/props/prop_syncer.h"           // Claim, AuthorsActorHere
#include "coop/props/prop_throw.h"            // SeqFor
#include "coop/session/net_pump.h"            // HasAnnouncedWorldReady
#include "ue_wrap/actors/character_push.h"   // MovementBaseOf
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_physics.h"    // IsComponentSimulatingPhysics

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::client_shove {

// The pure rules, checked where they are compiled.
static_assert(IsShove(true, true, false), "a simulating prop near the pawn and in no other lane's hands is a shove");
static_assert(!IsShove(false, true, false), "a kinematic prop is some other lane's");
static_assert(!IsShove(true, false, false), "a prop far from the pawn was not shoved by it");
static_assert(!IsShove(true, true, true), "a prop another lane has is not claimed");
static_assert(Within(299.f * 299.f, kShoveRangeCm) && !Within(301.f * 301.f, kShoveRangeCm), "the shove range is 300 cm");
static_assert(ClaimAllowed(0) && ClaimAllowed(kMaxClaimsPerWindow - 1) && !ClaimAllowed(kMaxClaimsPerWindow),
              "a window takes kMaxClaimsPerWindow claims");

namespace {

namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
namespace R  = ue_wrap::reflection;
using coop::element::Registry;

constexpr unsigned kMaxSaid = 12;   // lines said without the dev flag, per kind

struct Entry {
    void*             actor = nullptr;
    int32_t           idx   = -1;
    ue_wrap::FVector  baseLoc{};
    ue_wrap::FRotator baseRot{};
    ue_wrap::FVector  lastLoc{};     // the last place a read saw; picks the near or far set at the next refresh
    bool              hasBase  = false;
    bool              hasLoc   = false;
    bool              eligible = false;   // a keyed Aprop_C of a class the wire expresses; judged once per actor
    void*             mesh = nullptr;     // its StaticMesh, the component a pawn standing on it has as its base
};

std::unordered_map<uint32_t, Entry> g_entries;       // eid -> state, the host-allocated props this client has
std::vector<uint32_t> g_near, g_far;
size_t   g_nearCursor = 0, g_farCursor = 0;
uint64_t g_nextRefreshMs = 0;
std::vector<Registry::ActorIdPair> g_snap;           // kept for its capacity
std::unordered_map<uint32_t, uint64_t> g_reclaimAt;  // eid -> no new claim before this
std::unordered_set<uint32_t> g_mine;                 // eids this module has claimed in the current episode
uint64_t g_windowStartMs = 0;
uint32_t g_sentInWindow = 0;
size_t   g_claims = 0;
unsigned g_saidClaim = 0, g_saidHold = 0, g_saidStand = 0, g_saidSettle = 0;
size_t   g_settleSkips = 0;
size_t   g_standClaims = 0;
uint32_t g_standEid = 0;                             // the prop the local pawn stood on at the last tick, 0 for none
// A base no candidate has, until the next refresh: the far set is walked once per new base, so standing on the floor
// costs one walk a second.
void*    g_missBase = nullptr;
bool     g_enabled = true;

bool Verbose() {
    static const bool on = coop::config::ResolveFlag(coop::config_registry::rows::local_prop_push_log);
    return on;
}

bool PawnLocation(ue_wrap::FVector& out) {
    void* const pawn = coop::players::Registry::Get().Local();
    return pawn && R::IsLive(pawn) && E::TryGetActorLocation(pawn, out);
}

float Dist2(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz;
}

// The candidate lists, rebuilt from the Registry: the host-allocated Prop elements this client holds. Baselines of
// the props still here are kept; a recycled eid on a new actor starts over. Near or far by the last place a read saw.
void Refresh(const ue_wrap::FVector& pawn) {
    g_enabled = coop::config::ResolveFlag(coop::config_registry::rows::client_shove);
    Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, g_snap);
    std::unordered_map<uint32_t, Entry> next;
    next.reserve(g_snap.size());
    g_near.clear();
    g_far.clear();
    for (const auto& pr : g_snap) {
        if (!pr.actor || !Registry::IsAllowedHostAllocatedEid(pr.id)) continue;
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
            if (e.eligible) e.mesh = PR::GetStaticMesh(pr.actor);
        }
        if (!e.eligible) continue;
        (e.hasLoc && Within(Dist2(e.lastLoc, pawn), kNearCm) ? g_near : g_far).push_back(eid);
        next.emplace(eid, e);
    }
    g_entries.swap(next);
    g_missBase = nullptr;
    g_nearCursor = g_nearCursor < g_near.size() ? g_nearCursor : 0;
    g_farCursor  = g_farCursor  < g_far.size()  ? g_farCursor  : 0;
    for (auto it = g_reclaimAt.begin(); it != g_reclaimAt.end();)
        it = g_entries.count(it->first) ? std::next(it) : g_reclaimAt.erase(it);
    for (auto it = g_mine.begin(); it != g_mine.end();)
        it = g_entries.count(*it) ? std::next(it) : g_mine.erase(it);
}

// One read of one candidate. True when it sent a claim.
bool Check(uint32_t eid, const ue_wrap::FVector& pawn, uint64_t now) {
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
    e.lastLoc = loc;
    e.hasLoc = true;
    if (!e.hasBase) {
        e.baseLoc = loc;
        e.baseRot = rot;
        e.hasBase = true;
        return false;
    }
    if (!MovedPastShoveEps(loc, rot, e.baseLoc, e.baseRot)) return false;
    // Whatever the verdict, the baseline follows: a prop judged once is not judged again for the same motion.
    e.baseLoc = loc;
    e.baseRot = rot;

    const bool nearPawn = Within(Dist2(loc, pawn), kShoveRangeCm);
    void* mesh = PR::GetStaticMesh(e.actor);
    const bool simulating = mesh && E::IsComponentSimulatingPhysics(mesh);
    // The lanes that already own this prop's motion: a hand, the host's stream (its park, and the settling that
    // follows its End), a throw's or swing's election, a claim open here.
    const bool authored = coop::prop_syncer::AuthorsActorHere(e.actor);
    bool mine = g_mine.count(eid) != 0;
    if (mine && !authored) {   // the episode ended (a rest End, a refusal, a revoke): a new shove claims afresh
        g_mine.erase(eid);
        mine = false;
    }
    const bool otherLane = coop::prop_drive_host::InAHand(e.actor) || coop::prop_drive_host::IsDriven(e.actor) ||
                           coop::prop_drive_stream::IsParked(e.actor) || coop::prop_throw::SeqFor(e.actor) != 0 ||
                           coop::prop_drive_stream::LastEndAgeMs(eid) < kQuietAfterEndMs || (authored && !mine);
    if (!IsShove(simulating, nearPawn, otherLane)) return false;

    // A heap the host's stream is working (its Ends teleport and release bodies, its parks lerp them) jostles the
    // neighbours here. Their motion is that settling, not this pawn's shove: claimed, each hands its copy to this
    // peer's physics, whose release then jostles the next, and a workbench's pile never rests. A pawn within
    // kContactCm may really have touched it, so only a prop out of reach is spared.
    if (!mine && Dist2(loc, pawn) > kContactCm * kContactCm &&
        coop::prop_drive_stream::SettlingNear(loc, kSettleRadiusCm, kQuietAfterEndMs, eid)) {
        ++g_settleSkips;
        if (Verbose() || g_saidSettle < kMaxSaid) {
            ++g_saidSettle;
            UE_LOGI("[CLIENT-SHOVE] eid=%u moved %.1f cm at (%.1f,%.1f,%.1f), %.0f cm from the pawn, beside a prop the "
                    "host's stream is settling: the pile's motion, not claimed (#%zu skipped)", eid,
                    static_cast<double>(std::sqrt(Dist2(loc, e.baseLoc))), static_cast<double>(loc.X),
                    static_cast<double>(loc.Y), static_cast<double>(loc.Z),
                    static_cast<double>(std::sqrt(Dist2(loc, pawn))), g_settleSkips);
        }
        return false;
    }

    // An episode this module opened that goes on: the interaction is refreshed (idempotent, no send, no token).
    if (mine && authored) {
        coop::prop_syncer::Claim(e.actor, coop::net::PropSyncReason::LocalShove);
        return false;
    }
    if (auto r = g_reclaimAt.find(eid); r != g_reclaimAt.end() && now < r->second) return false;
    if (now - g_windowStartMs >= kWindowMs) {
        g_windowStartMs = now;
        g_sentInWindow = 0;
    }
    if (!ClaimAllowed(g_sentInWindow)) {
        if (Verbose() || g_saidHold < kMaxSaid) {
            ++g_saidHold;
            UE_LOGI("[CLIENT-SHOVE] eid=%u moved %.1f cm near the pawn but the claim pace (%u per %llu ms) holds it back",
                    eid, static_cast<double>(std::sqrt(Dist2(loc, pawn))), kMaxClaimsPerWindow,
                    static_cast<unsigned long long>(kWindowMs));
        }
        return false;
    }
    ++g_sentInWindow;
    g_reclaimAt[eid] = now + kReclaimMs;
    g_mine.insert(eid);
    ++g_claims;
    if (Verbose() || g_saidClaim < kMaxSaid) {
        ++g_saidClaim;
        UE_LOGI("[CLIENT-SHOVE] claim eid=%u at (%.1f,%.1f,%.1f), %.0f cm from the pawn (#%zu)", eid,
                static_cast<double>(loc.X), static_cast<double>(loc.Y), static_cast<double>(loc.Z),
                static_cast<double>(std::sqrt(Dist2(loc, pawn))), g_claims);
    }
    coop::prop_syncer::Claim(e.actor, coop::net::PropSyncReason::LocalShove);
    return true;
}

// The prop the local pawn stands on: the eid of the near candidate whose StaticMesh is the pawn's movement base, 0 when
// the pawn stands on no candidate (the floor, a mirror, a prop this client authors as no host element, or nothing).
uint32_t Find(const std::vector<uint32_t>& list, void* base) {
    for (const uint32_t eid : list) {
        auto it = g_entries.find(eid);
        if (it != g_entries.end() && it->second.mesh == base && R::IsLiveByIndex(it->second.actor, it->second.idx))
            return eid;
    }
    return 0;
}

uint32_t StoodOn(void* pawn) {
    void* base = ue_wrap::character_push::MovementBaseOf(pawn);
    if (!base) return 0;
    if (const uint32_t eid = Find(g_near, base)) return eid;
    if (base == g_missBase) return 0;
    if (const uint32_t eid = Find(g_far, base)) return eid;
    g_missBase = base;
    return 0;
}

// The prop the local pawn stands on is this client's to sync, for as long as it stands there (one physics authority
// per body: the standing body's own peer, whose capsule rests on its copy). Without this, a still player let the
// claim's rest End go out, the host released its copy and coasted the release's settle back, the client's copy under
// the player's feet was parked and moved by the host's stream, and the next shove claimed it again -- the prop
// changed hands every few seconds, each hand-over a park, a pose snap and a re-simulate under the capsule. Standing
// claims without waiting for motion (a crouch-jump lands before any shove reads), skips the settle window after a host
// End (standing is no settle), and refreshes the open episode every tick, so its rest End waits until the pawn steps
// off. A prop another lane has (a hand, the host's stream, a throw, the host's own pawn standing on it, which the
// host refuses) is left to it; the stand is asked again each tick, the pacing bounding the sends.
void CheckStand(uint32_t eid, uint64_t now) {
    auto it = g_entries.find(eid);
    if (it == g_entries.end()) return;
    Entry& e = it->second;
    void* mesh = e.mesh;
    if (!mesh || !E::IsComponentSimulatingPhysics(mesh)) return;
    const bool authored = coop::prop_syncer::AuthorsActorHere(e.actor);
    bool mine = g_mine.count(eid) != 0;
    if (mine && !authored) {
        g_mine.erase(eid);
        mine = false;
    }
    if (mine) {   // the episode goes on while the pawn stands: its rest End waits
        coop::prop_syncer::Claim(e.actor, coop::net::PropSyncReason::LocalShove);
        return;
    }
    if (authored || coop::prop_drive_host::InAHand(e.actor) || coop::prop_drive_host::IsDriven(e.actor) ||
        coop::prop_drive_stream::IsParked(e.actor) || coop::prop_throw::SeqFor(e.actor) != 0)
        return;
    if (auto r = g_reclaimAt.find(eid); r != g_reclaimAt.end() && now < r->second) return;
    if (now - g_windowStartMs >= kWindowMs) {
        g_windowStartMs = now;
        g_sentInWindow = 0;
    }
    if (!ClaimAllowed(g_sentInWindow)) return;
    ++g_sentInWindow;
    g_reclaimAt[eid] = now + kReclaimMs;
    g_mine.insert(eid);
    ++g_claims;
    ++g_standClaims;
    if (Verbose() || g_saidStand < kMaxSaid) {
        ++g_saidStand;
        UE_LOGI("[CLIENT-SHOVE] stand claim eid=%u: the local pawn stands on it (#%zu)", eid, g_standClaims);
    }
    coop::prop_syncer::Claim(e.actor, coop::net::PropSyncReason::LocalShove);
}

void Sweep(std::vector<uint32_t>& list, size_t& cursor, size_t reads, const ue_wrap::FVector& pawn, uint64_t now) {
    const size_t n = list.size();
    if (n == 0) return;
    if (reads > n) reads = n;
    for (size_t k = 0; k < reads; ++k) {
        if (cursor >= n) cursor = 0;
        Check(list[cursor++], pawn, now);
    }
}

}  // namespace

void Tick(coop::net::Session& /*s*/, bool isHost) {
    UE_ASSERT_GAME_THREAD("client_shove::Tick");
    if (isHost) return;
    // A claim sent before the announce is dropped by the claim path and counted; nothing to do until it is up.
    if (!coop::net_pump::HasAnnouncedWorldReady()) return;
    ue_wrap::FVector pawn{};
    if (!PawnLocation(pawn)) return;
    const uint64_t now = coop::active_drive::NowMs();
    if (now >= g_nextRefreshMs) {
        Refresh(pawn);
        g_nextRefreshMs = now + kRefreshMs;
    }
    if (!g_enabled) return;
    const uint32_t stood = StoodOn(coop::players::Registry::Get().Local());
    if (stood != g_standEid && Verbose())
        UE_LOGI("[CLIENT-SHOVE] the local pawn's base: eid=%u (was %u)", stood, g_standEid);
    g_standEid = stood;
    if (stood) CheckStand(stood, now);
    Sweep(g_near, g_nearCursor, kNearChecksPerTick, pawn, now);
    Sweep(g_far, g_farCursor, kFarChecksPerTick, pawn, now);
}

size_t ClaimsSent() {
    return g_claims;
}

void OnDisconnect() {
    if (g_claims || g_settleSkips)
        UE_LOGI("[CLIENT-SHOVE] session end: %zu claim(s) sent for a prop this client's body shoved or stood on (%zu stand), "
                "%zu move(s) beside a settling pile not claimed", g_claims, g_standClaims, g_settleSkips);
    g_entries.clear();
    g_near.clear();
    g_far.clear();
    g_snap.clear();
    g_reclaimAt.clear();
    g_mine.clear();
    g_nearCursor = g_farCursor = 0;
    g_nextRefreshMs = 0;
    g_windowStartMs = 0;
    g_sentInWindow = 0;
    g_claims = 0;
    g_saidClaim = g_saidHold = g_saidStand = g_saidSettle = 0;
    g_settleSkips = 0;
    g_standClaims = 0;
    g_standEid = 0;
    g_missBase = nullptr;
    g_enabled = true;
}

}  // namespace coop::client_shove
