// coop/session/teleport_to_player.cpp -- /tpto: an admin moved beside another player. See
// coop/session/teleport_client.h (TeleportToPlayer).

#include "coop/session/teleport_client.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/anti_noclip_rescue.h"
#include "coop/player/players_registry.h"
#include "coop/player/pocket_travel_host.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdint>

namespace coop::teleport_client {
namespace {

namespace E = ue_wrap::engine;
using ue_wrap::FVector;

constexpr float kPi = 3.14159265f;
// The places tried around the target, as degrees from the way it faces (behind first), at two radii.
constexpr float kAnglesDeg[] = {180.f, 90.f, -90.f, 0.f, 135.f, -135.f, 45.f, -45.f};
constexpr float kRadiiCm[] = {150.f, 230.f};
// A place whose floor is further than this from the target's centre is another storey, not beside it.
constexpr float kMaxFloorDeltaCm = 160.f;

// Whether `slot` still holds the occupancy `generation` (the host, slot 0, always does).
bool Holds(const coop::net::Session& s, int slot, uint32_t generation) {
    if (slot == 0) return true;
    return slot >= 1 && slot < coop::net::kMaxPeers && generation != 0 && s.peerGenerationForSlot(slot) == generation;
}

// Where the target stands and which way it faces; false when the pose cannot be read.
bool TargetPose(int targetSlot, void* hostPawn, FVector& pos, float& yawDeg) {
    if (targetSlot == 0) {
        ue_wrap::FRotator rot{};
        if (!E::TryGetActorLocation(hostPawn, pos) || !E::TryGetActorRotation(hostPawn, rot)) return false;
        yawDeg = rot.Yaw;
        return true;
    }
    coop::RemotePlayer* puppet = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(targetSlot));
    if (!puppet || !puppet->TryGetLocation(pos)) return false;
    yawDeg = puppet->GetSyncedAimRotation().Yaw;
    return true;
}

// The first place around `pos` a body can stand and that nothing walls off from `pos`.
bool ChooseSpot(void* hostPawn, const FVector& pos, float yawDeg, FVector& stand) {
    for (const float radius : kRadiiCm) {
        for (const float angle : kAnglesDeg) {
            const float rad = (yawDeg + angle) * kPi / 180.f;
            const FVector at{pos.X + std::cos(rad) * radius, pos.Y + std::sin(rad) * radius, pos.Z};
            if (ue_wrap::trace::LineBlockedStatDyn(hostPawn, pos, at) != 0) continue;   // a wall, or unknown
            FVector floorCentre{};
            if (!coop::anti_noclip_rescue::StandingAt(hostPawn, at, floorCentre)) continue;
            if (std::fabs(floorCentre.Z - pos.Z) > kMaxFloorDeltaCm) continue;
            stand = floorCentre;
            return true;
        }
    }
    return false;
}

}  // namespace

ToPlayer TeleportToPlayer(coop::net::Session& s, int callerSlot, uint32_t callerGen, int targetSlot,
                          uint32_t targetGen) {
    UE_ASSERT_GAME_THREAD("teleport_client::TeleportToPlayer");
    if (!s.running() || s.role() != coop::net::Role::Host) return ToPlayer::NoSession;
    if (callerSlot == targetSlot) return ToPlayer::SameBody;
    if (!Holds(s, callerSlot, callerGen)) return ToPlayer::CallerGone;
    if (!Holds(s, targetSlot, targetGen)) return ToPlayer::TargetGone;
    namespace PT = coop::player::pocket_travel_host;
    // A slot in a pocket dimension, or one whose world is not up, has no place in this level to go to or
    // be taken from.
    if (callerSlot > 0 && (PT::IsSlotIsolated(static_cast<uint8_t>(callerSlot)) || !s.IsSlotWorldReady(callerSlot)))
        return ToPlayer::CallerAway;
    if (targetSlot > 0) {
        if (PT::IsSlotIsolated(static_cast<uint8_t>(targetSlot))) return ToPlayer::TargetAway;
        if (!s.IsSlotWorldReady(targetSlot)) return ToPlayer::TargetJoining;
        if (coop::players::Registry::Get().LifeOf(static_cast<uint8_t>(targetSlot)) == coop::players::PuppetLife::Dead)
            return ToPlayer::TargetDead;
    }
    void* hostPawn = coop::players::Registry::Get().Local();
    if (!hostPawn) return ToPlayer::Failed;
    FVector pos{};
    float yawDeg = 0.f;
    if (!TargetPose(targetSlot, hostPawn, pos, yawDeg)) return ToPlayer::Failed;
    FVector stand{};
    if (!ChooseSpot(hostPawn, pos, yawDeg, stand)) return ToPlayer::NoSpot;
    // Face the target from where the caller will stand.
    const float faceYaw = std::atan2(pos.Y - stand.Y, pos.X - stand.X) * 180.f / kPi;
    if (callerSlot == 0) {
        const ApplyArgs a{stand.X, stand.Y, stand.Z, 0.f, faceYaw, 0.f};
        return ApplyLocally(a) ? ToPlayer::Done : ToPlayer::Failed;
    }
    coop::net::TeleportClientPayload p{};
    p.locX = stand.X; p.locY = stand.Y; p.locZ = stand.Z;
    p.rotPitch = 0.f; p.rotYaw = faceYaw; p.rotRoll = 0.f;
    if (!s.SendReliableToSlot(callerSlot, coop::net::ReliableKind::TeleportClient, &p, sizeof(p))) {
        UE_LOGW("teleport_client: /tpto send to slot %d failed (not connected?)", callerSlot);
        return ToPlayer::Failed;
    }
    UE_LOGI("teleport_client: /tpto slot %d -> beside slot %d at (%.0f,%.0f,%.0f)", callerSlot, targetSlot,
            stand.X, stand.Y, stand.Z);
    return ToPlayer::Done;
}

}  // namespace coop::teleport_client
