// coop/world/event_scene_crawler.cpp -- the vent crawler's walk-in, any player's (see coop/world/event_scene_sync.h).
//
// ventCrawler_C waits for its Box's begin-overlap, a DoOnce with no actor filter (the 0.10.0 capture's
// BndEvt__ventCrawler_Box handler): two vent footsteps, then its crawl. The host's crawler is the only one that
// acts (a client's mirror has the handler refused, coop/world/spawn_authority), so only an overlap on the host
// starts it, and a client's puppet there may never overlap the box: the puppet's own components ignore the
// PhysicsBody channel, which keeps a puppet from shoving props (ue_wrap/actors/puppet_spawn). That guard stays.
// Instead, while a host crawler lives and has not been started by us, the host tests each client's puppet against
// the box (its rotated bounds, padded by a body's half width) four times a second, and the first one inside runs
// the box's own handler with that puppet as the other actor, once. The handler's DoOnce makes a later native
// overlap, or a second puppet, a no-op. The same pass arms the sound relay while a crawler lives, so its
// footsteps and the vent's break are heard on every client. Host only; game thread.

#include "event_scene_detail.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/world/event_sound_relay.h"

#include "ue_wrap/actors/event_scenes.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace coop::event_scene_sync::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace P  = ue_wrap::profile;
namespace E  = ue_wrap::engine;
namespace ES = ue_wrap::event_scenes;

constexpr long long kPollMs = 250;
constexpr float kBodyPadCm = 40.f;   // a player capsule's half width: the location is the capsule's centre
long long g_nextPollMs = 0;
std::vector<coop::element::Registry::ActorIdPair> g_snap;   // kept for its capacity

// The crawlers this pass started, by slot and serial, so a recycled address is a new crawler.
struct Started {
    int32_t idx;
    int32_t serial;
};
std::vector<Started> g_started;

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool WasStarted(int32_t idx) {
    const int32_t serial = R::SlotSerial(idx);
    for (const Started& s : g_started)
        if (s.idx == idx && s.serial == serial) return true;
    return false;
}

// Whether `p` lies in the box at `c` with half extent `e` turned by `r`, padded by `pad`: `p - c` in the box's own
// axes (FRotationMatrix's columns), against the extent.
bool InsideBox(const ue_wrap::FVector& p, const ue_wrap::FVector& c, const ue_wrap::FVector& e,
               const ue_wrap::FRotator& r, float pad) {
    constexpr float kDegToRad = 0.017453292519943f;
    const float sp = std::sin(r.Pitch * kDegToRad), cp = std::cos(r.Pitch * kDegToRad);
    const float sy = std::sin(r.Yaw * kDegToRad),   cy = std::cos(r.Yaw * kDegToRad);
    const float sr = std::sin(r.Roll * kDegToRad),  cr = std::cos(r.Roll * kDegToRad);
    const float dx = p.X - c.X, dy = p.Y - c.Y, dz = p.Z - c.Z;
    const float lx = dx * (cp * cy) + dy * (cp * sy) + dz * sp;
    const float ly = dx * (sr * sp * cy - cr * sy) + dy * (sr * sp * sy + cr * cy) + dz * (-sr * cp);
    const float lz = dx * (-(cr * sp * cy + sr * sy)) + dy * (cy * sr - cr * sp * sy) + dz * (cr * cp);
    return std::fabs(lx) <= e.X + pad && std::fabs(ly) <= e.Y + pad && std::fabs(lz) <= e.Z + pad;
}

// The first client puppet inside the crawler's box, or null.
void* PuppetInBox(void* crawler) {
    ue_wrap::FVector c{}, e{};
    ue_wrap::FRotator r{};
    if (!ES::ReadVentCrawlerBox(crawler, c, e, r)) return nullptr;
    auto& reg = coop::players::Registry::Get();
    for (uint8_t slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        coop::RemotePlayer* rp = reg.Puppet(slot);
        void* body = rp ? rp->GetActor() : nullptr;
        ue_wrap::FVector at{};
        if (!body || !R::IsLive(body) || !E::TryGetActorLocation(body, at)) continue;
        if (InsideBox(at, c, e, r, kBodyPadCm)) return body;
    }
    return nullptr;
}

}  // namespace

void CrawlerHostTick() {
    const long long now = NowMs();
    if (now < g_nextPollMs) return;
    g_nextPollMs = now + kPollMs;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Npc, g_snap);
    bool anyCrawler = false;
    for (const auto& pr : g_snap) {
        if (pr.mirror || !pr.actor || !R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;
        if (!R::NameEquals(R::NameOf(R::ClassOf(pr.actor)), P::name::NpcClass_VentCrawler)) continue;
        anyCrawler = true;
        if (WasStarted(pr.internalIdx)) continue;
        void* puppet = PuppetInBox(pr.actor);
        if (!puppet) continue;
        g_started.push_back(Started{pr.internalIdx, R::SlotSerial(pr.internalIdx)});
        const bool ok = ES::CallVentCrawlerOverlap(pr.actor, puppet);
        UE_LOGI("event_scene[ventCrawler]: eid=%u -- a client's player is in the vent box: its overlap run on the "
                "host%s", static_cast<unsigned>(pr.id), ok ? "" : " -- the handler did not dispatch");
    }
    if (!anyCrawler) g_started.clear();
    coop::event_sound_relay::SetHostArmed(anyCrawler);
}

void CrawlerOnDisconnect() {
    g_started.clear();
    g_nextPollMs = 0;
}

}  // namespace coop::event_scene_sync::detail
