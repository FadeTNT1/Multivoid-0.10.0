// ue_wrap/actors/fire_extinguisher.cpp -- see ue_wrap/actors/fire_extinguisher.h.

#include "ue_wrap/actors/fire_extinguisher.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace ue_wrap::fire_extinguisher {
namespace {

namespace R = reflection;
namespace P = profile;

R::FName g_extName{0, 0};
R::FName g_mountName{0, 0};
bool     g_namesResolved = false;

// A member's place in the instance, read off the first live object of its class.
struct Member {
    int32_t offset = -1;
    uint8_t mask = 0;   // a bool's mask; 0 for a non-bool
    bool    tried = false;
};
Member g_mountHeld, g_charge, g_spraying, g_thrusting, g_thruster, g_thrustFx, g_thrustSound;

bool SameName(const R::FName& a, const R::FName& b) {
    return a.ComparisonIndex == b.ComparisonIndex && a.Number == b.Number;
}

bool IsA(void* obj, const R::FName& name) {
    if (!obj || !g_namesResolved) return false;
    void* cls = R::ClassOf(obj);
    for (int hops = 0; hops < 16 && cls; ++hops) {
        if (SameName(R::NameOf(cls), name)) return true;
        cls = R::SuperStructOf(cls);
    }
    return false;
}

// `type` is the property class the member must have (an FFieldClass name, "FloatProperty"), so a member that moved
// or changed kind between builds is a miss, not a misread. A bool is checked by its own lookup.
bool Resolve(Member& m, void* obj, const wchar_t* name, bool isBool, const wchar_t* type = nullptr) {
    if (m.offset >= 0) return true;
    if (m.tried) return false;
    m.tried = true;
    void* cls = R::ClassOf(obj);
    if (isBool) {
        int32_t off = -1;
        uint8_t mask = 0;
        if (!R::FindBoolProperty(cls, name, off, mask) || off < 0 || mask == 0) return false;
        m.offset = off;
        m.mask = mask;
        return true;
    }
    int32_t off = -1;
    std::wstring found;
    if (!R::PropertyOffsetAndClass(cls, name, off, found) || off < 0) return false;
    if (type && found != type) return false;
    m.offset = off;
    return true;
}

bool WriteBool(void* ext, Member& m, const wchar_t* name, bool value) {
    if (!IsExtinguisher(ext) || !Resolve(m, ext, name, /*isBool=*/true)) return false;
    uint8_t& byte = reinterpret_cast<uint8_t*>(ext)[m.offset];
    byte = value ? static_cast<uint8_t>(byte | m.mask) : static_cast<uint8_t>(byte & ~m.mask);
    return true;
}

// The component `name` names on `ext`, or null when it is not one, did not resolve or is not live.
void* ComponentOf(void* ext, Member& m, const wchar_t* name) {
    if (!IsExtinguisher(ext) || !Resolve(m, ext, name, /*isBool=*/false, L"ObjectProperty")) return nullptr;
    void* comp = nullptr;
    std::memcpy(&comp, reinterpret_cast<const uint8_t*>(ext) + m.offset, sizeof(comp));
    return comp && R::IsLive(comp) ? comp : nullptr;
}

bool ReadBool(void* ext, Member& m, const wchar_t* name, bool& out) {
    if (!IsExtinguisher(ext) || !Resolve(m, ext, name, /*isBool=*/true)) return false;
    out = (reinterpret_cast<const uint8_t*>(ext)[m.offset] & m.mask) != 0;
    return true;
}

}  // namespace

bool ResolveNames() {
    if (g_namesResolved) return true;
    const R::FName ext = fname_utils::StringToFName(P::name::FireExtinguisherClass);
    const R::FName mount = fname_utils::StringToFName(P::name::FireExtinguisherMountClass);
    if (ext.ComparisonIndex == 0 || mount.ComparisonIndex == 0) return false;
    g_extName = ext;
    g_mountName = mount;
    g_namesResolved = true;
    return true;
}

bool IsExtinguisher(void* obj) { return IsA(obj, g_extName); }
bool IsMount(void* obj) { return IsA(obj, g_mountName); }

void* MountedExtinguisher(void* mount) {
    if (!IsMount(mount) || !Resolve(g_mountHeld, mount, P::name::FireExtMountHeldProp, false, L"ObjectProperty"))
        return nullptr;
    return *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(mount) + g_mountHeld.offset);
}

bool ReadCharge(void* ext, float& out) {
    if (!IsExtinguisher(ext) || !Resolve(g_charge, ext, P::name::FireExtChargeProp, false, L"FloatProperty")) return false;
    out = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(ext) + g_charge.offset);
    return true;
}

bool ReadSpraying(void* ext, bool& out) {
    return ReadBool(ext, g_spraying, P::name::FireExtSprayingProp, out);
}

bool ReadThrusting(void* ext, bool& out) {
    return ReadBool(ext, g_thrusting, P::name::FireExtThrustingProp, out);
}

bool WriteCharge(void* ext, float value) {
    if (!IsExtinguisher(ext) || !Resolve(g_charge, ext, P::name::FireExtChargeProp, false, L"FloatProperty")) return false;
    std::memcpy(reinterpret_cast<uint8_t*>(ext) + g_charge.offset, &value, sizeof(value));
    return true;
}

bool WriteThrusting(void* ext, bool value) {
    return WriteBool(ext, g_thrusting, P::name::FireExtThrustingProp, value);
}

bool CallShoot(void* ext) {
    if (!IsExtinguisher(ext)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(ext), P::name::FireExtShootFn);
    if (!fn) return false;
    ParamFrame frame(fn);
    return frame.valid() && Call(ext, frame);
}

bool StopThrust(void* ext) {
    void* comps[3] = {ComponentOf(ext, g_thruster, P::name::FireExtThrusterComp),
                      ComponentOf(ext, g_thrustFx, P::name::FireExtThrustFxComp),
                      ComponentOf(ext, g_thrustSound, P::name::FireExtThrustSoundComp)};
    bool any = false;
    for (void* comp : comps) {
        if (!comp) continue;
        void* fn = R::FindDispatchFunctionCached(R::ClassOf(comp), P::name::ComponentDeactivateFn);
        if (!fn) continue;
        ParamFrame frame(fn);
        if (frame.valid() && Call(comp, frame)) any = true;
    }
    return any;
}

}  // namespace ue_wrap::fire_extinguisher
