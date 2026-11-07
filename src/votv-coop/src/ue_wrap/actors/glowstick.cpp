// ue_wrap/actors/glowstick.cpp -- see ue_wrap/actors/glowstick.h.

#include "ue_wrap/actors/glowstick.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine_component.h"

#include <cstdint>

namespace ue_wrap::glowstick {
namespace {

namespace R = reflection;
namespace N = profile::name;

// A bool member's byte and mask on the actor's own class. Linear walk, but asked only on a stick edge, a verb the
// game ran once, or a joiner's sweep, never per frame.
bool ReadBoolMember(void* actor, const wchar_t* name, bool& out) {
    if (!IsGlowstick(actor)) return false;
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(actor), name, off, mask) || off < 0 || mask == 0) return false;
    out = (*(reinterpret_cast<const uint8_t*>(actor) + off) & mask) != 0;
    return true;
}

bool WriteBoolMember(void* actor, const wchar_t* name, bool on) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(actor), name, off, mask) || off < 0 || mask == 0) return false;
    uint8_t& byte = *(reinterpret_cast<uint8_t*>(actor) + off);
    byte = on ? static_cast<uint8_t>(byte | mask) : static_cast<uint8_t>(byte & ~mask);
    return true;
}

bool CallNoParam(void* actor, const wchar_t* fnName) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(actor), fnName);
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && Call(actor, f);
}

}  // namespace

bool IsGlowstick(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    void* base = object_index::ClassByName(N::GlowstickClass);
    void* cls = R::ClassOf(actor);
    if (!base || !cls) return false;
    void* bases[] = {base};
    return R::IsDescendantOfAny(cls, bases, 1);
}

bool ReadSticked(void* actor, bool& out) {
    return ReadBoolMember(actor, N::GlowstickStickedProp, out);
}

bool ReadStickStatic(void* actor, bool& out) {
    return ReadBoolMember(actor, N::GlowstickStickStaticProp, out);
}

bool ReadHookPoint(void* actor, FVector& out) {
    if (!IsGlowstick(actor)) return false;
    static R::InstanceOffset sLoc(N::GlowstickLocProp);
    const int32_t off = sLoc.Of(actor);
    if (off < 0) return false;
    out = *reinterpret_cast<const FVector*>(static_cast<const uint8_t*>(actor) + off);
    return true;
}

bool CallHang(void* actor, const FVector& hookPoint) {
    if (!IsGlowstick(actor)) return false;
    static R::InstanceOffset sLoc(N::GlowstickLocProp);
    static R::InstanceOffset sHook(N::GlowstickHookProp);
    const int32_t locOff = sLoc.Of(actor);
    const int32_t hookOff = sHook.Of(actor);
    if (locOff < 0 || hookOff < 0) return false;
    void* hook = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(actor) + hookOff);
    if (!hook || !R::IsLive(hook)) return false;
    // stickto's tail for a hit on geometry that does not simulate, in the same order: the members, the hook at the
    // impact point, then hookdelay (which constrains the hook to the stick's mesh and sets the frames).
    if (!WriteBoolMember(actor, N::GlowstickStickedProp, true) ||
        !WriteBoolMember(actor, N::GlowstickStickStaticProp, true))
        return false;
    *reinterpret_cast<FVector*>(static_cast<uint8_t*>(actor) + locOff) = hookPoint;
    if (!engine::SetComponentWorldLocation(hook, hookPoint)) return false;
    return CallNoParam(actor, N::GlowstickHookDelayFn);
}

bool CallRelease(void* actor) {
    if (!IsGlowstick(actor)) return false;
    return CallNoParam(actor, N::GlowstickReleaseFn);
}

}  // namespace ue_wrap::glowstick
