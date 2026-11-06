// coop/interactables/door_crack.cpp -- see coop/interactables/door_crack.h.

#include "coop/interactables/door_crack.h"

#include "coop/interactables/interactable_sync.h"  // DoorKey, for the log lines

#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/door.h"

#include <algorithm>
#include <cmath>

namespace coop::interactable_sync::door_crack {
namespace {

namespace D = ue_wrap::door;

constexpr float kPlaceTolerance = 0.003f;  // the wire's per-mille step is 0.001; two steps is the same place
constexpr uint32_t kCookiePositioned = 1u;  // a moving seed: the swing was started and put at the host's place

uint16_t Permille(float f) {
    return static_cast<uint16_t>(std::lround(std::fmin(1.f, std::fmax(0.f, f)) * 1000.f));
}

float FromPermille(uint16_t v) { return static_cast<float>(std::min<uint16_t>(v, 1000)) / 1000.f; }

// A shut door at rest whose roots stand off their rest place: their per-mille. A door that is swinging, jam-shaking
// or open is the bool lane's, and a read that fails says nothing.
bool RestCrack(void* door, uint16_t& left, uint16_t& right) {
    bool moving = false, open = false;
    D::LeafPose pose;
    if (!D::TryReadMoving(door, moving) || moving || !D::TryReadOpen(door, open) || open ||
        !D::TryReadLeafPose(door, pose))
        return false;
    left = Permille(pose.left);
    right = Permille(pose.right);
    return left > 0 || right > 0;
}

bool SamePlace(const D::LeafPose& a, const D::LeafPose& b) {
    return std::fabs(a.left - b.left) <= kPlaceTolerance && std::fabs(a.right - b.right) <= kPlaceTolerance;
}

}  // namespace

uint32_t DetailOf(void* door) {
    uint16_t l = 0, r = 0;
    if (!RestCrack(door, l, r)) return 0;
    return (1u << 20) | (static_cast<uint32_t>(l) << 10) | r;
}

void FillPayload(void* door, const std::wstring& key, bool /*on*/, coop::net::KeyedTogglePayload& p) {
    uint16_t l = 0, r = 0;
    if (!RestCrack(door, l, r)) return;
    p.travel = l;
    p.holdGen = r;
    p.seedFlags = coop::net::kSeedFlagSeed | coop::net::kSeedFlagCrack;
    UE_LOGI("door: sent crack key='%ls' r=%.3f l=%.3f", key.c_str(), FromPermille(r), FromPermille(l));
}

void FillSeed(void* door, bool /*on*/, coop::net::KeyedTogglePayload& p) {
    D::SwingPose swing;
    if (!D::TryReadSwing(door, swing)) return;
    p.travel = Permille(swing.position);
    p.holdGen = Permille(swing.anchor.left);
    p.seedFlags = coop::net::kSeedFlagSeed | coop::net::kSeedFlagMoving;
    UE_LOGI("door: sent swing key='%ls' toward %s at %.3f from r=%.3f l=%.3f", DoorKey(door).c_str(),
            swing.toOpen ? "open" : "shut", FromPermille(p.travel), swing.anchor.right, swing.anchor.left);
}

bool IsSeed(const coop::net::KeyedTogglePayload& p) { return (p.seedFlags & coop::net::kSeedFlagSeed) != 0; }

SeedStep ApplySeed(void* door, bool on, const coop::net::KeyedTogglePayload& p, uint32_t& cookie) {
    bool moving = false, open = false;
    if (!D::TryReadMoving(door, moving) || !D::TryReadOpen(door, open)) return SeedStep::Failed;

    if (p.seedFlags & coop::net::kSeedFlagMoving) {
        if (cookie & kCookiePositioned) {
            if (moving) return SeedStep::Retry;           // the swing is running from the host's place
            if (open == on) return SeedStep::Rest;
            cookie &= ~kCookiePositioned;                 // the swing did not complete: start it again
            return SeedStep::Retry;
        }
        D::SmartApply(door, on);                          // the usual swing; idempotent for one already heading there
        if (!D::TryReadMoving(door, moving) || !moving) {
            D::TryReadOpen(door, open);
            return open == on ? SeedStep::Rest : SeedStep::Retry;
        }
        D::SwingPose swing;
        swing.toOpen = on;
        swing.position = FromPermille(p.travel);
        swing.anchor.left = swing.anchor.right = FromPermille(p.holdGen);
        if (!D::SeedSwing(door, swing)) return SeedStep::Failed;
        cookie |= kCookiePositioned;
        UE_LOGI("door: applied swing key='%ls' toward %s at %.3f from r=%.3f l=%.3f", DoorKey(door).c_str(),
                on ? "open" : "shut", swing.position, swing.anchor.right, swing.anchor.left);
        return SeedStep::Retry;
    }

    if ((p.seedFlags & coop::net::kSeedFlagCrack) && !on) {
        if (moving) return SeedStep::Retry;               // a swing on this copy ends first
        if (open) {                                       // the host's door is shut: shut this one, then crack it
            D::SmartApply(door, false);
            return SeedStep::Retry;
        }
        D::LeafPose want, cur;
        want.left = FromPermille(p.travel);
        want.right = FromPermille(p.holdGen);
        if (!D::TryReadLeafPose(door, cur)) return SeedStep::Failed;
        if (SamePlace(cur, want)) return SeedStep::Rest;
        if (!D::WriteLeafPose(door, want)) return SeedStep::Failed;
        UE_LOGI("door: applied crack key='%ls' r=%.3f l=%.3f", DoorKey(door).c_str(), want.right, want.left);
        return SeedStep::Rest;
    }
    return SeedStep::Failed;  // a seed this lane does not drive: the state lands the ordinary way
}

}  // namespace coop::interactable_sync::door_crack
