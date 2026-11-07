// ue_wrap/actors/ufo_ball.cpp -- see ue_wrap/actors/ufo_ball.h.

#include "ue_wrap/actors/ufo_ball.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine_component.h"

#include <cstdint>

namespace ue_wrap::ufo_ball {
namespace {

namespace R = reflection;
namespace P = profile;
namespace E = engine;

R::InstanceOffset g_center{P::name::UfoBallCenterProp};
R::InstanceOffset g_a{P::name::UfoBallAProp};
R::InstanceOffset g_b{P::name::UfoBallBProp};
R::InstanceOffset g_balls{P::name::UfoBallBallsProp};

// The live component in the pointer slot `off` bytes into `base`, or null.
void* ComponentAt(void* base, int32_t off) {
    if (!base || off < 0) return nullptr;
    void* p = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(base) + static_cast<size_t>(off));
    return (p && R::IsLive(p)) ? p : nullptr;
}

struct Parts { void* center; void* a; void* b; void* balls; };

bool Resolve(void* actor, Parts& out) {
    if (!IsUfoBall(actor)) return false;
    out.center = ComponentAt(actor, g_center.Of(actor));
    out.a      = ComponentAt(actor, g_a.Of(actor));
    out.b      = ComponentAt(actor, g_b.Of(actor));
    out.balls  = ComponentAt(actor, g_balls.Of(actor));
    if (out.center && out.a && out.b && out.balls) return true;
    static bool s_said = false;
    if (!s_said) {
        s_said = true;
        UE_LOGW("ufo_ball: a component did not resolve on %p (center=%p a=%p b=%p balls=%p) -- the ball's setup "
                "cannot cross", actor, out.center, out.a, out.b, out.balls);
    }
    return false;
}

}  // namespace

bool IsUfoBall(void* actor) {
    return actor && R::IsLive(actor) && R::NameEquals(R::NameOf(R::ClassOf(actor)), P::name::UfoBallClass);
}

bool Read(void* actor, Setup& out) {
    Parts p{};
    if (!Resolve(actor, p)) return false;
    FVector balls{};
    if (!E::TryGetComponentLocation(p.balls, balls)) return false;
    // center's relative yaw: the raw member (FRotator Pitch, Yaw, Roll), as the other relative reads here.
    const float yaw = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(p.center) +
                                                      P::off::USceneComponent_RelativeRotation + sizeof(float));
    const FVector a = E::GetComponentRelativeLocation(p.a);
    const FVector b = E::GetComponentRelativeLocation(p.b);
    out.centerYaw = yaw;
    out.aX = a.X; out.aY = a.Y;
    out.bX = b.X; out.bY = b.Y;
    out.balls = balls;
    return true;
}

bool Apply(void* actor, const Setup& in) {
    Parts p{};
    if (!Resolve(actor, p)) return false;
    bool ok = E::SetComponentRelativeRotation(p.center, FRotator{0.f, in.centerYaw, 0.f});
    ok = E::SetComponentRelativeLocation(p.a, FVector{in.aX, in.aY, 0.f}) && ok;
    ok = E::SetComponentRelativeLocation(p.b, FVector{in.bX, in.bY, 0.f}) && ok;
    ok = E::SetComponentWorldLocation(p.balls, in.balls) && ok;
    return ok;
}

}  // namespace ue_wrap::ufo_ball
