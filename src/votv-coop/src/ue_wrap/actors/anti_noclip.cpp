// ue_wrap/actors/anti_noclip.cpp -- see ue_wrap/actors/anti_noclip.h.

#include "ue_wrap/actors/anti_noclip.h"

#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <cstdint>
#include <cstring>

namespace ue_wrap::anti_noclip {
namespace {

namespace R = reflection;
namespace P = profile;

void* CapsuleOf(void* player) {
    if (!player || !R::IsLive(player)) return nullptr;
    void* capsule = nullptr;
    std::memcpy(&capsule, reinterpret_cast<const char*>(player) + P::off::ACharacter_CapsuleComponent, sizeof(capsule));
    return capsule && R::IsLive(capsule) ? capsule : nullptr;
}

}  // namespace

bool ReadLastLoc(void* player, FVector& out) {
    void* cls = player ? R::ClassOf(player) : nullptr;
    if (!cls) return false;
    static void*   sCls = nullptr;
    static int32_t sOff = -1;
    static bool    sSaid = false;
    if (cls != sCls) {
        sCls = cls;
        sOff = R::FindPropertyOffset(cls, P::name::MainPlayerLastLocProp);
        if (sOff < 0 && !sSaid) {
            sSaid = true;
            UE_LOGW("anti_noclip: %ls.%ls did not resolve -- the anti-noclip rescue is left out",
                    P::name::MainPlayerClass, P::name::MainPlayerLastLocProp);
        }
    }
    if (sOff < 0) return false;
    std::memcpy(&out, reinterpret_cast<const char*>(player) + sOff, sizeof(FVector));
    return true;
}

bool WriteLastLoc(void* player, const FVector& at) {
    FVector probe{};
    if (!ReadLastLoc(player, probe)) return false;   // resolves the offset, said once when it does not
    void* cls = R::ClassOf(player);
    const int32_t off = R::FindPropertyOffset(cls, P::name::MainPlayerLastLocProp);
    if (off < 0) return false;
    std::memcpy(reinterpret_cast<char*>(player) + off, &at, sizeof(FVector));
    return true;
}

bool CapsuleResponse(void* player, uint8_t channel, uint8_t& out) {
    void* capsule = CapsuleOf(player);
    return capsule && engine::GetCollisionResponseToChannel(capsule, channel, out);
}

bool CapsuleHalfHeight(void* player, float& out) {
    void* capsule = CapsuleOf(player);
    if (!capsule) return false;
    float h = 0.f;
    std::memcpy(&h, reinterpret_cast<const char*>(capsule) + P::off::UCapsuleComponent_CapsuleHalfHeight, sizeof(h));
    if (!(h > 1.f && h < 1000.f)) return false;
    out = h;
    return true;
}

bool ViewTargetIsSelf(void* player, bool& out) {
    void* ctrl = player ? engine::GetController(player) : nullptr;
    void* cls = ctrl ? R::ClassOf(ctrl) : nullptr;
    void* fn = cls ? R::FindDispatchFunctionCached(cls, P::name::GetViewTargetFn) : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid() || !Call(ctrl, f)) return false;
    out = f.Get<void*>(L"ReturnValue") == player;
    return true;
}

bool CallCanBackrooms(void* gamemode, void* fn, bool& out) {
    if (!gamemode || !fn) return false;
    ParamFrame f(fn);
    if (!f.valid() || f.ParamOffset(P::name::CanBackroomsReturnParam) < 0 || !Call(gamemode, f)) return false;
    out = f.Get<bool>(P::name::CanBackroomsReturnParam);
    return true;
}

bool TeleportStill(void* player, const FVector& loc) {
    void* cls = player ? R::ClassOf(player) : nullptr;
    void* fn = cls ? R::FindDispatchFunctionCached(cls, P::name::MainPlayerTeleportFn) : nullptr;
    if (!fn) return false;
    // The verb takes the velocity from the transform's scale when `useVelocity` is set: a zero scale is no velocity.
    FTransform xform = MakeTransform(loc);
    xform.SX = xform.SY = xform.SZ = 0.f;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    f.SetRaw(P::name::TeleportNewTransformParam, &xform, sizeof(xform));
    f.Set<bool>(P::name::TeleportUseRotationParam, false);
    f.Set<bool>(P::name::TeleportTrueRotationParam, false);
    f.Set<bool>(P::name::TeleportUseVelocityParam, true);
    f.Set<bool>(P::name::TeleportControlRotationParam, false);
    if (!Call(player, f)) return false;
    puppet::DriveCharacterMovement(player, FVector{}, /*inAir*/ false);   // belt and braces: no velocity, walking
    return true;
}

}  // namespace ue_wrap::anti_noclip
