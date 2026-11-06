// coop/element/mirror_defer.cpp -- see coop/element/mirror_defer.h. Instant-world deferred-spawn
// visibility.

#include "coop/element/mirror_defer.h"

#include "ue_wrap/engine/engine.h"          // SetActorHiddenInGame + SetActorEnableCollision
#include "ue_wrap/engine/engine_attach.h"   // a mirror's primitives: OwnPrimitiveComponents, Get/SetComponentCollisionEnabled
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"      // InternalIndexOf + IsLiveByIndex

#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace coop::mirror_defer {
namespace {

namespace R = ue_wrap::reflection;

// One hidden host mirror. `idx` is the captured InternalIndex so reveal-time liveness is the
// GC-robust IsLiveByIndex (the raw actor ptr can be freed by a sweep that destroys a ghost
// between hide and reveal -- never deref it first). `collision` records what the hide did to
// collision (so reveal restores exactly that); for PhysicsOnly `comps` holds each primitive the hide
// changed, with the GetCollisionEnabled it found, and `primitives` counts the actor's own primitives the
// hide walked. `fallbackWhy` is why a PhysicsOnly hide became Off (null when it did not). `hold` = held
// to quiescence.
struct CompRec { ue_wrap::CachedObjRef comp; int prev; };
struct HiddenRec {
    void* actor; int32_t idx; HideCollision collision; std::vector<CompRec> comps; int primitives;
    const char* fallbackWhy; bool hold;
};

// The most primitives a PhysicsOnly hide takes; an actor with more falls back to Off.
constexpr int kMaxHiddenComps = 16;

// eid -> hidden record. This IS the authoritative hidden set: a mirror is hidden ONLY via
// OnMirrorSpawned, which records here, so walking this map at quiescence reveals everything we
// hid and nothing can be left stuck hidden -- by construction, not by a sweep that looks for
// strays. Game-thread only (no mutex).
std::unordered_map<uint32_t, HiddenRec> g_hidden;
// eids already revealed this join -- OnMirrorSpawned must NEVER re-hide one. A trash spawn is
// idempotent for the same eid, so a post-lift re-spawn convergence of an already-revealed
// confirmed mirror would otherwise vanish it until quiescence. The rule
// is: do not re-hide on the idempotent re-spawn return, for any mirror kind.
std::unordered_set<uint32_t> g_revealed;
bool g_armed = false;

// The watchdog's clocks. Every reveal above is driven by a message or a latch (SnapshotComplete, the
// quiescence probe); if the drive never comes -- the handler that carries it faulted partway, or the
// latch never lands -- the mirrors it owes a reveal stay hidden for good: a prop the host sees and
// the joiner does not. So the window also ages out on its own. Game thread only.
using Clock = std::chrono::steady_clock;
Clock::time_point g_armedAt{};
Clock::time_point g_lastHideAt{};
bool g_liftDone = false;  // RevealConfirmedAtLift ran in this window
// With no lift reveal and no new hidden mirror for this long, the bracket is not coming: show the
// confirmed ones. The host's bracket arrives within seconds of its begin; a slow link keeps hiding
// (each hide restarts the clock), so only a quiet one trips it.
constexpr std::chrono::seconds kLiftIdleLimit{45};
// A window open this long is over, whatever else is pending: the absolute bound of the quiescence
// probe is 120 s, and this sits past it.
constexpr std::chrono::seconds kWindowLimit{180};

// Reveal one record: Show + restore what the hide changed of collision (Off: the actor flag back on;
// PhysicsOnly: each primitive's value the hide found, for those still live). Liveness-gated -- a record
// whose actor was destroyed by the quiescence sweep (a ghost) is simply dropped, never touched.
void RevealRec(const HiddenRec& r) {
    if (!R::IsLiveByIndex(r.actor, r.idx)) return;  // swept ghost / GC'd -> nothing to reveal
    ue_wrap::engine::SetActorHiddenInGame(r.actor, false);
    switch (r.collision) {
    case HideCollision::Keep:
        break;
    case HideCollision::Off:
        ue_wrap::engine::SetActorEnableCollision(r.actor, true);
        break;
    case HideCollision::PhysicsOnly:
        for (const CompRec& c : r.comps)
            if (void* comp = c.comp.Get()) ue_wrap::engine::SetComponentCollisionEnabled(comp, static_cast<uint8_t>(c.prev));
        break;
    }
}

// PhysicsOnly for all of an actor's own primitives, or nothing: every primitive is read first, and the hide
// changes none unless all read, the root has the physics facet and they fit the bound. A component with the
// physics bit (2, 3) becomes 2, a query-only one (1) becomes 0, one with none stays. `why` names the refusal
// (null on success); a write that fails is undone.
bool HidePhysicsOnly(void* actor, std::vector<CompRec>& changed, int& primitives, const char*& why) {
    void* comps[kMaxHiddenComps];
    const int n = ue_wrap::engine::OwnPrimitiveComponents(actor, comps, kMaxHiddenComps);
    primitives = n > 0 ? n : 0;
    if (n < 0) { why = "more primitives than the cap, or the walk's bound"; return false; }
    if (n == 0) { why = "the root is not a primitive"; return false; }
    int prev[kMaxHiddenComps];
    for (int i = 0; i < n; ++i) {
        prev[i] = ue_wrap::engine::GetComponentCollisionEnabled(comps[i]);
        if (prev[i] < 0) { why = "a primitive's collision did not read"; return false; }
    }
    if ((prev[0] & 2) == 0) { why = "the root has no physics facet"; return false; }
    for (int i = 0; i < n; ++i) {
        const int target = (prev[i] & 2) ? 2 : 0;
        if (target == prev[i]) continue;
        CompRec rec;
        rec.comp.Set(comps[i]);
        rec.prev = prev[i];
        if (!ue_wrap::engine::SetComponentCollisionEnabled(comps[i], static_cast<uint8_t>(target))) {
            for (const CompRec& c : changed)
                if (void* comp = c.comp.Get()) ue_wrap::engine::SetComponentCollisionEnabled(comp, static_cast<uint8_t>(c.prev));
            changed.clear();
            why = "a primitive's collision did not write";
            return false;
        }
        changed.push_back(rec);
    }
    return true;
}

}  // namespace

void Arm() {
    UE_ASSERT_GAME_THREAD("mirror_defer::Arm");
    // Reveal any stale hidden mirrors from a PRIOR bracket before re-arming (a soft re-bracket -- two
    // SnapshotBegin without a disconnect in between -- would otherwise strand them invisible). Liveness-gated,
    // so a full teardown's dead actors are a harmless no-op. Same reveal-then-clear discipline as Reset().
    for (auto& kv : g_hidden) RevealRec(kv.second);
    g_armed = true;
    g_armedAt = Clock::now();
    g_lastHideAt = g_armedAt;
    g_liftDone = false;
    g_hidden.clear();
    g_revealed.clear();
    UE_LOGI("mirror_defer: ARMED -- join deferred-hide window open (new host mirrors spawn hidden "
            "until lift/quiescence reveal; reconcile backup untouched)");
}

void Reset() {
    // Abort / mid-drain re-bracket / teardown. REVEAL everything still alive FIRST so a hidden mirror is
    // never stranded invisible (on a soft re-bracket the world persists; on a full teardown the actors are
    // dead -> liveness-gated reveal is a harmless no-op), then drop all tracking + disarm.
    for (auto& kv : g_hidden) RevealRec(kv.second);
    g_armed = false;
    g_hidden.clear();
    g_revealed.clear();
}

void OnMirrorSpawned(uint32_t eid, void* actor, HideCollision collision, bool holdUntilQuiescence) {
    UE_ASSERT_GAME_THREAD("mirror_defer::OnMirrorSpawned");
    if (!g_armed || !actor || eid == 0u) return;
    if (g_revealed.count(eid)) return;  // already shown this join -> never re-hide (idempotent re-spawn)
    const int32_t idx = R::InternalIndexOf(actor);
    const auto prior = g_hidden.find(eid);
    if (prior != g_hidden.end()) {
        // The same actor again: the first record stands (it holds what the mirror had before any hide). A
        // different actor under this eid: the old one is shown and restored before the new one is hidden.
        if (prior->second.actor == actor && prior->second.idx == idx) return;
        RevealRec(prior->second);
        g_hidden.erase(prior);
    }
    ue_wrap::engine::SetActorHiddenInGame(actor, true);
    std::vector<CompRec> comps;
    int primitives = 0;
    const char* fallbackWhy = nullptr;
    if (collision == HideCollision::PhysicsOnly && !HidePhysicsOnly(actor, comps, primitives, fallbackWhy)) {
        // The actor-level Off, today's hide: nothing of PhysicsOnly was applied, and the reveal restores the flag.
        UE_LOGW("mirror_defer: eid=%u fell back to Off: %s (%d primitives walked)", eid, fallbackWhy, primitives);
        collision = HideCollision::Off;
    }
    if (collision == HideCollision::Off) ue_wrap::engine::SetActorEnableCollision(actor, false);
    g_hidden[eid] = HiddenRec{actor, idx, collision, std::move(comps), primitives, fallbackWhy, holdUntilQuiescence};
    g_lastHideAt = Clock::now();
}

bool HideInfoOf(uint32_t eid, HideInfo& out) {
    const auto it = g_hidden.find(eid);
    if (it == g_hidden.end()) return false;
    out = HideInfo{it->second.collision, it->second.primitives, it->second.fallbackWhy};
    return true;
}

void RevealConfirmedAtLift() {
    UE_ASSERT_GAME_THREAD("mirror_defer::RevealConfirmedAtLift");
    if (!g_armed) {
        UE_LOGI("mirror_defer: lift-reveal skipped -- the deferred-hide window is not open (nothing is held hidden)");
        return;
    }
    g_liftDone = true;
    int revealed = 0;
    for (auto it = g_hidden.begin(); it != g_hidden.end();) {
        if (it->second.hold) { ++it; continue; }  // local twin still visible -> wait for quiescence
        RevealRec(it->second);
        g_revealed.insert(it->first);
        it = g_hidden.erase(it);
        ++revealed;
    }
    UE_LOGI("mirror_defer: lift-reveal -- %d confirmed mirror(s) shown, %zu held to quiescence "
            "(the assembled world is now visible; the uncertain tail stays hidden)",
            revealed, g_hidden.size());
}

void RevealAllSurvivorsAtQuiescence() {
    UE_ASSERT_GAME_THREAD("mirror_defer::RevealAllSurvivorsAtQuiescence");
    if (!g_armed) return;
    const int n = static_cast<int>(g_hidden.size());
    for (auto& kv : g_hidden) RevealRec(kv.second);
    g_hidden.clear();
    g_armed = false;
    UE_LOGI("mirror_defer: quiescence-reveal -- %d held/tail mirror(s) shown; deferred-hide window "
            "CLOSED (steady-state spawns now visible immediately)", n);
}

void TickWatchdog() {
    UE_ASSERT_GAME_THREAD("mirror_defer::TickWatchdog");
    if (!g_armed) return;
    const auto now = Clock::now();
    if (now - g_armedAt >= kWindowLimit) {
        const int n = static_cast<int>(g_hidden.size());
        for (auto& kv : g_hidden) RevealRec(kv.second);
        g_hidden.clear();
        g_armed = false;
        UE_LOGW("mirror_defer: watchdog -- the window was open %lld s with no quiescence reveal; %d hidden "
                "mirror(s) shown and the window CLOSED (a mirror held for its twin may now be visible beside it)",
                static_cast<long long>(kWindowLimit.count()), n);
        return;
    }
    if (!g_liftDone && !g_hidden.empty() && now - g_lastHideAt >= kLiftIdleLimit) {
        int revealed = 0;
        for (auto it = g_hidden.begin(); it != g_hidden.end();) {
            if (it->second.hold) { ++it; continue; }
            RevealRec(it->second);
            g_revealed.insert(it->first);
            it = g_hidden.erase(it);
            ++revealed;
        }
        g_liftDone = true;  // once: the window's own reveals take it from here
        UE_LOGW("mirror_defer: watchdog -- no lift reveal and no new mirror for %lld s (the bracket's end never "
                "drove it); %d confirmed mirror(s) shown, %zu held to quiescence",
                static_cast<long long>(kLiftIdleLimit.count()), revealed, g_hidden.size());
    }
}

}  // namespace coop::mirror_defer
