// coop/world/event_scene_tower.cpp -- the radio tower's ariral ship (arirShip_tower_C), the host's alone (see
// coop/world/event_scene_sync.h).
//
// The ship is a level-placed actor every peer loads. Its BeginPlay loops on a 5 s delay (the 0.10.0 capture's
// arirShip_tower_C receivebeginplay entry): while its `visible` is set it calls appear(false); otherwise, on day 20
// or later, at hour 21 to 3, a 10 % roll, with player 0 in its `box` and its `sphere_0` unseen for a fifth of a
// second, it calls appear(true). appear(dir) fades the ship in or out and, on the way in, plays the vehicletp sound,
// a camera shake and the `press` push on props near it; the fade-out's finish mints a ufodreamer_C and destroys it.
// Each peer ran its own loop, so each rolled for its own player 0 on its own 5 s clock: a client walking up the
// tower saw a ship the host never had, and the host's came to a client that stood elsewhere only if it stood there
// too.
//
// Now the host's loop is the only one. A client refuses every appear its own loop makes (event_scene_refuse); the
// host's appear is a scene phase each client replays on its own copy, so the fade, the sound and the shake start
// together and the fade-out follows the host's by the same five seconds. A client's player at the tower is rolled
// here, on the host, by the ship's own rule: the host tests each client's puppet against the box (the puppet never
// overlaps it, see event_scene_crawler) on the same 5 s cadence and, past the night, day, roll and unseen checks,
// calls the ship's own appear(true), which the phase then carries to everyone. The unseen check reads the HOST's
// camera; a client's view is not known here. Host and client; game thread.

#include "event_scene_detail.h"

#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/actors/event_scenes.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/daynightcycle.h"

#include <chrono>
#include <cstdint>
#include <random>

namespace coop::event_scene_sync::detail {
namespace {

namespace R   = ue_wrap::reflection;
namespace P   = ue_wrap::profile;
namespace E   = ue_wrap::engine;
namespace ES  = ue_wrap::event_scenes;
namespace DNC = ue_wrap::daynightcycle;

constexpr long long kRollMs = 5000;    // the ship's own loop delay
constexpr float kBodyPadCm  = 40.f;    // a player capsule's half width: the location is the capsule's centre

long long g_nextRollMs = 0;
bool g_applying = false;               // a client is running the host's phase through appear
std::mt19937 g_rng{std::random_device{}()};

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The ship's own night rule: day >= 20, and hour >= 21 or <= 3.
bool NightRuleHolds() {
    int32_t hour = 0, minute = 0, day = 0;
    if (!DNC::ReadTimeZ(hour, minute, day)) return false;
    return day >= P::name::kArirTowerMinDay &&
           (hour >= P::name::kArirTowerNightFrom || hour <= P::name::kArirTowerNightTo);
}

bool AnyPuppetInBox(void* tower) {
    ue_wrap::FVector c{}, e{};
    ue_wrap::FRotator r{};
    if (!ES::ReadArirTowerBox(tower, c, e, r)) return false;
    auto& reg = coop::players::Registry::Get();
    for (uint8_t slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        coop::RemotePlayer* rp = reg.Puppet(slot);
        void* body = rp ? rp->GetActor() : nullptr;
        ue_wrap::FVector at{};
        if (!body || !R::IsLive(body) || !E::TryGetActorLocation(body, at)) continue;
        if (InsideBox(at, c, e, r, kBodyPadCm)) return true;
    }
    return false;
}

}  // namespace

void ArirTowerHostTick() {
    const long long now = NowMs();
    if (now < g_nextRollMs) return;
    g_nextRollMs = now + kRollMs;
    auto* s = Session();
    if (!s || !s->AnyWorldReadyPeer()) return;
    void* tower = ES::FindFirstLevelActor(P::name::ArirTowerClass);
    if (!tower) return;
    bool visible = true;
    if (!ES::ReadArirTowerVisible(tower, visible) || visible) return;
    if (!NightRuleHolds() || !AnyPuppetInBox(tower)) return;
    if (std::uniform_real_distribution<float>(0.f, 1.f)(g_rng) >= P::name::kArirTowerChance) return;
    bool seen = false;
    if (ES::ArirTowerSeen(tower, P::name::kArirTowerSeenWindow, seen) && seen) return;
    // The ship's own verb; the watch on appear records the host's phase and the lane sends it.
    const bool ok = ES::ArirTowerAppear(tower, true);
    // The watch on appear has seen it already when the engine routes the call through the gate; a second note of the
    // same phase is dropped, so this one only covers a route that skips the gate.
    if (ok) NoteHostPhase(coop::net::event_scene_ids::kArirTower, coop::net::event_scene_phases::kTowerAppear, tower);
    UE_LOGI("event_scene[arirTower]: a client's player is at the tower and the roll hit: appear(true) run on the "
            "host%s", ok ? "" : " -- the verb did not dispatch");
}

bool ArirTowerClientApply(void* actor, bool shown) {
    g_applying = true;
    const bool ok = ES::ArirTowerAppear(actor, shown);
    g_applying = false;
    return ok;
}

bool ArirTowerApplying() { return g_applying; }

void ArirTowerOnDisconnect() {
    g_nextRollMs = 0;
    g_applying = false;
}

}  // namespace coop::event_scene_sync::detail
