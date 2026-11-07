// coop/world/ufo_ball_sync.cpp -- see coop/world/ufo_ball_sync.h.

#include "coop/world/ufo_ball_sync.h"

#include "coop/net/protocol.h"

#include "ue_wrap/actors/ufo_ball.h"
#include "ue_wrap/core/log.h"

#include <cmath>
#include <cstring>

namespace coop::ufo_ball_sync {
namespace {

namespace UB = ue_wrap::ufo_ball;

constexpr float kMaxRelative = 1.0e6f;   // an endpoint's relative offset: far past the class's `size`
constexpr float kMaxYaw      = 720.f;    // a relative yaw: the class rolls 0..360

bool Finite(float v) { return std::isfinite(v); }

}  // namespace

bool IsBallActor(void* actor) { return UB::IsUfoBall(actor); }

void FillBirth(void* actor, unsigned int eid, uint8_t* blob, size_t cap, uint8_t& len) {
    len = 0;
    if (!blob || cap < kBirthBytes) return;
    UB::Setup s{};
    if (!UB::Read(actor, s)) {
        UE_LOGW("ufo_ball[host]: eid=%u -- setup UNREADABLE, sending no birth content. The mirror will fly its own "
                "random course, not the host's", eid);
        return;
    }
    const float v[8] = {s.centerYaw, s.aX, s.aY, s.bX, s.bY, s.balls.X, s.balls.Y, s.balls.Z};
    std::memcpy(blob, v, sizeof(v));
    len = static_cast<uint8_t>(sizeof(v));
    UE_LOGI("ufo_ball[host]: eid=%u yaw=%.1f a=(%.0f,%.0f) b=(%.0f,%.0f) balls=(%.0f,%.0f,%.0f)", eid, s.centerYaw,
            s.aX, s.aY, s.bX, s.bY, s.balls.X, s.balls.Y, s.balls.Z);
}

void SeedMirror(void* actor, unsigned int eid, const uint8_t* blob, size_t len) {
    if (len == 0) {
        UE_LOGW("ufo_ball[client]: eid=%u -- no birth content; the mirror flies its own random course", eid);
        return;
    }
    if (len != kBirthBytes || !blob) {
        UE_LOGW("ufo_ball[client]: eid=%u birthLen=%zu, expected %zu -- dropping the setup (a protocol violation)", eid,
                len, kBirthBytes);
        return;
    }
    float v[8];
    std::memcpy(v, blob, sizeof(v));
    for (float f : v) {
        if (!Finite(f)) {
            UE_LOGW("ufo_ball[client]: eid=%u non-finite setup -- dropping it", eid);
            return;
        }
    }
    if (std::fabs(v[0]) > kMaxYaw) {
        UE_LOGW("ufo_ball[client]: eid=%u yaw %.1f out of range -- dropping the setup", eid, v[0]);
        return;
    }
    for (int i = 1; i < 5; ++i) {
        if (std::fabs(v[i]) > kMaxRelative) {
            UE_LOGW("ufo_ball[client]: eid=%u endpoint offset %.0f out of range -- dropping the setup", eid, v[i]);
            return;
        }
    }
    for (int i = 5; i < 8; ++i) {
        if (std::fabs(v[i]) > coop::net::kMaxCoord) {
            UE_LOGW("ufo_ball[client]: eid=%u ball position %.0f out of bounds -- dropping the setup", eid, v[i]);
            return;
        }
    }
    UB::Setup s{};
    s.centerYaw = v[0];
    s.aX = v[1]; s.aY = v[2];
    s.bX = v[3]; s.bY = v[4];
    s.balls = ue_wrap::FVector{v[5], v[6], v[7]};
    if (!UB::Apply(actor, s)) {
        UE_LOGW("ufo_ball[client]: eid=%u -- the setup did not apply (a component did not resolve or a setter failed); "
                "the mirror flies its own random course", eid);
        return;
    }
    UE_LOGI("ufo_ball[client]: eid=%u seeded yaw=%.1f a=(%.0f,%.0f) b=(%.0f,%.0f) balls=(%.0f,%.0f,%.0f)", eid,
            s.centerYaw, s.aX, s.aY, s.bX, s.bY, s.balls.X, s.balls.Y, s.balls.Z);
}

}  // namespace coop::ufo_ball_sync
