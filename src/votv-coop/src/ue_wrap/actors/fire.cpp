// ue_wrap/actors/fire.cpp -- see ue_wrap/actors/fire.h.

#include "ue_wrap/actors/fire.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstdint>
#include <cstring>

namespace ue_wrap::fire {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

// Each field's offset on the one class that declares it, held by its class's slot and serial (InstanceOffset).
R::InstanceOffset g_fuel{P::name::FireFuelProp};
R::InstanceOffset g_fuelSpeed{P::name::FireFuelSpeedProp};
R::InstanceOffset g_fireSize{P::name::FireSizeProp};
R::InstanceOffset g_effectRate{P::name::FireEffectRateProp};
R::InstanceOffset g_igniteChance{P::name::FireIgniteChanceProp};
R::InstanceOffset g_boxExtent{P::name::FireBoxExtentProp};

R::InstanceOffset g_compOwner{P::name::FlammableOwnerProp};
R::InstanceOffset g_compBurning{P::name::FlammableBurningProp};
R::InstanceOffset g_compFuelSpeed{P::name::FlammableFuelSpeedProp};
R::InstanceOffset g_compIgniteChance{P::name::FlammableIgniteChanceProp};

R::InstanceOffset g_flameBase{P::name::GasolineFlameBaseProp};

template <class T>
bool ReadAt(void* obj, R::InstanceOffset& field, T& out) {
    if (!obj) return false;
    const int32_t off = field.Of(obj);
    if (off < 0) return false;
    std::memcpy(&out, reinterpret_cast<const uint8_t*>(obj) + off, sizeof(T));
    return true;
}

template <class T>
bool WriteAt(void* obj, R::InstanceOffset& field, const T& value) {
    if (!obj) return false;
    const int32_t off = field.Of(obj);
    if (off < 0) return false;
    std::memcpy(reinterpret_cast<uint8_t*>(obj) + off, &value, sizeof(T));
    return true;
}

bool ClassNamed(void* obj, const wchar_t* name) {
    if (!obj) return false;
    void* cls = R::ClassOf(obj);
    return cls && R::NameEquals(R::NameOf(cls), name);
}

}  // namespace

bool IsFireClass(void* cls) { return cls && R::NameEquals(R::NameOf(cls), P::name::FireClass); }

bool IsFire(void* actor) { return ClassNamed(actor, P::name::FireClass); }

bool IsGasolineGrime(void* actor) { return ClassNamed(actor, P::name::GasolineGrimeClass); }

bool ReadParams(void* fire, Params& out) {
    return ReadAt(fire, g_fuel, out.fuel) && ReadAt(fire, g_fuelSpeed, out.fuelSpeed) &&
           ReadAt(fire, g_fireSize, out.fireSize) && ReadAt(fire, g_effectRate, out.effectRate) &&
           ReadAt(fire, g_igniteChance, out.igniteChance) && ReadAt(fire, g_boxExtent, out.boxExtent);
}

bool WriteParams(void* fire, const Params& p) {
    // Every offset first, so a class missing one field is left untouched rather than half written.
    if (!fire || g_fuel.Of(fire) < 0 || g_fuelSpeed.Of(fire) < 0 || g_fireSize.Of(fire) < 0 ||
        g_effectRate.Of(fire) < 0 || g_igniteChance.Of(fire) < 0 || g_boxExtent.Of(fire) < 0)
        return false;
    return WriteAt(fire, g_fuel, p.fuel) && WriteAt(fire, g_fuelSpeed, p.fuelSpeed) &&
           WriteAt(fire, g_fireSize, p.fireSize) && WriteAt(fire, g_effectRate, p.effectRate) &&
           WriteAt(fire, g_igniteChance, p.igniteChance) && WriteAt(fire, g_boxExtent, p.boxExtent);
}

void* FlammableOwner(void* comp) {
    void* owner = nullptr;
    if (!ReadAt(comp, g_compOwner, owner) || !owner || !R::IsLive(owner)) return nullptr;
    return owner;
}

bool FlammableBurning(void* comp) {
    bool burning = false;
    return ReadAt(comp, g_compBurning, burning) && burning;
}

bool ParkFlammable(void* comp, float fuelSpeed, float igniteChance) {
    if (!comp || g_compFuelSpeed.Of(comp) < 0 || g_compIgniteChance.Of(comp) < 0) return false;
    return WriteAt(comp, g_compFuelSpeed, fuelSpeed) && WriteAt(comp, g_compIgniteChance, igniteChance);
}

bool CallExtinguish(void* comp) {
    if (!comp) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(comp), P::name::FlammableExtinguishFn);
    if (!fn) return false;
    ParamFrame frame(fn);
    return frame.valid() && Call(comp, frame);
}

void* GasolineFlameBase(void* grime) {
    void* comp = nullptr;
    if (!ReadAt(grime, g_flameBase, comp) || !comp || !R::IsLive(comp)) return nullptr;
    return comp;
}

bool CallGasolineIgnite(void* grime, float fuel) {
    if (!grime) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(grime), P::name::GasolineIgniteFn);
    if (!fn) return false;
    ParamFrame frame(fn);
    return frame.valid() && frame.Set(P::name::GasolineIgniteFuelParam, fuel) && Call(grime, frame);
}

}  // namespace ue_wrap::fire
