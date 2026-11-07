// coop/world/event_scene_agrav.cpp -- the anti-gravity prank's floating props near the clients (see
// coop/world/event_scene_sync.h).
//
// trigger_agrav_C's gaher gathers the props within 10000 of player 0's camera, the host's, and its runTrigger turns
// their gravity off, one each `duration / count` seconds, nudging each up 10 cm/s and spinning it 1-5 deg/s, then
// 5-7 s later turns every one back on, before its cloak(false). The host is the physics authority of every
// shared prop, so a client's copies follow on the prop lane: the motion scan coasts a floating prop, and the
// stream's end carries the body's gravity (propspawn_flags::kNoGravity), so a copy at rest in the air stays there.
// A client standing elsewhere saw nothing float around it: these are the props near each client's puppet that the
// game's gather, centred on the host, left out. At the cloak's on edge the host floats them the same way, all at
// once, and at its off edge gives them their gravity back. Host only; game thread.
//
// The game's own end turns gravity back on and wakes nothing. A body that came to rest while floating (the 10 cm/s
// push damps out) is asleep in PhysX, and SetEnableGravity(true) does not wake it, so it hangs in the air until
// something hits or lifts it; and the host's stream only follows a body that moves, so the clients' copies hang
// too. The off edge therefore gives back every body the event touched -- the game's own `comps` list and the ones
// this module floated -- gravity on and woken. A client replays the same edge for the copies the host's stream
// left without gravity (prop_wire_parity's gravity watch).

#include "event_scene_detail.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "coop/props/prop_wire_parity.h"

#include "ue_wrap/actors/event_scenes.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/engine_physics.h"

#include <cstdint>
#include <vector>

namespace coop::event_scene_sync::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace P  = ue_wrap::profile;
namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
namespace ES = ue_wrap::event_scenes;

// The props this module floated, to be given their gravity back.
struct Floated {
    void*   actor;
    int32_t idx;
};
std::vector<Floated> g_floated;
std::vector<coop::element::Registry::ActorIdPair> g_snap;   // kept for its capacity
constexpr size_t kMaxFloated = 256;   // a busy base's props near a client, well past the game's own gather

uint32_t g_rng = 0x9E3779B9u;
float Rand01() {
    g_rng = g_rng * 1664525u + 1013904223u;
    return static_cast<float>(g_rng >> 8) / 16777216.f;
}

float Dist2(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz;
}

void FloatNearClients() {
    const float r2 = P::name::kAgravGatherRadiusCm * P::name::kAgravGatherRadiusCm;
    const ue_wrap::FVector host = E::GetCameraLocation();
    ue_wrap::FVector clients[coop::players::kMaxPeers];
    int nClients = 0;
    auto& reg = coop::players::Registry::Get();
    for (uint8_t slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        coop::RemotePlayer* rp = reg.Puppet(slot);
        void* body = rp ? rp->GetActor() : nullptr;
        ue_wrap::FVector loc{};
        if (body && R::IsLive(body) && E::TryGetActorLocation(body, loc)) clients[nClients++] = loc;
    }
    if (nClients == 0) return;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, g_snap);
    for (const auto& pr : g_snap) {
        if (g_floated.size() >= kMaxFloated) break;
        if (pr.mirror || !pr.actor || !R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;
        ue_wrap::FVector loc{};
        if (!E::TryGetActorLocation(pr.actor, loc)) continue;
        if (Dist2(loc, host) <= r2) continue;   // the game's own gather has it
        bool near = false;
        for (int i = 0; i < nClients && !near; ++i) near = Dist2(loc, clients[i]) <= r2;
        if (!near) continue;
        void* mesh = PR::GetStaticMesh(pr.actor);
        // A frozen, static or sleeping prop does not simulate, and the game's own float leaves it as it is.
        if (!mesh || !E::IsComponentSimulatingPhysics(mesh)) continue;
        if (!E::SetComponentEnableGravity(mesh, false)) continue;
        E::SetComponentLinearVelocity(mesh, 0.f, 0.f, 10.f);
        const float spin = 1.f + 4.f * Rand01();
        E::SetComponentAngularVelocity(mesh, (Rand01() - 0.5f) * 2.f * spin, (Rand01() - 0.5f) * 2.f * spin,
                                       (Rand01() - 0.5f) * 2.f * spin);
        g_floated.push_back(Floated{pr.actor, pr.internalIdx});
    }
    UE_LOGI("event_scene[agrav]: %zu prop(s) near %d client(s) floated with the host's own", g_floated.size(),
            nClients);
}

// Gravity on and the body woken for every prop this module floated.
void Restore() {
    size_t restored = 0, woken = 0;
    for (const Floated& f : g_floated) {
        if (!R::IsLiveByIndex(f.actor, f.idx)) continue;
        const E::GravityRelease r = E::ReleaseComponentGravity(PR::GetStaticMesh(f.actor));
        if (r.gravityRestored) ++restored;
        if (r.woken) ++woken;
    }
    if (!g_floated.empty())
        UE_LOGI("event_scene[agrav]: gravity back on %zu and %zu woken of %zu prop(s) near the clients", restored, woken,
                g_floated.size());
    g_floated.clear();
}

// The off edge: the game's own float list gets what it was owed too. Its restore loop has finished by the time
// cloak(false) runs, so what is still without gravity here is a body it never reached (one destroyed and rebuilt, a
// prop the host's other lanes touched) and what is asleep is every body that came to rest in the air.
void ReleaseGameList(void* trigger) {
    static std::vector<void*> comps;   // kept for its capacity
    comps.clear();
    const size_t n = ES::AgravGatheredComponents(trigger, comps);
    size_t restored = 0, woken = 0;
    for (void* c : comps) {
        const E::GravityRelease r = E::ReleaseComponentGravity(c);
        if (r.gravityRestored) ++restored;
        if (r.woken) ++woken;
    }
    UE_LOGI("event_scene[agrav]: end: %zu of the game's %zu gathered body(ies) given gravity back, %zu woken", restored,
            n, woken);
}

}  // namespace

void AgravHostEdge(void* trigger, bool on) {
    if (!IsHost()) return;
    if (on) {
        Restore();   // a second cloak(true) before its off edge: the first set lands first
        FloatNearClients();
    } else {
        Restore();
        ReleaseGameList(trigger);
    }
}

void AgravClientEdge(bool on) {
    if (!IsActiveClient()) return;
    if (on) {
        coop::prop_wire_parity::OpenGravityWatch();
        return;
    }
    const coop::prop_wire_parity::GravityWatchResult r = coop::prop_wire_parity::ReleaseWatchedGravity();
    UE_LOGI("event_scene[agrav]: end: %zu of %zu copy(ies) the host floated given gravity back, %zu woken", r.restored,
            r.watched, r.woken);
}

void AgravOnDisconnect() {
    // The session's end leaves the host alone: a prop still floating for a client that left falls as the game's
    // own would have. Woken as the end's would have, and a client's watch is closed.
    Restore();
    coop::prop_wire_parity::ReleaseWatchedGravity();
}

}  // namespace coop::event_scene_sync::detail
