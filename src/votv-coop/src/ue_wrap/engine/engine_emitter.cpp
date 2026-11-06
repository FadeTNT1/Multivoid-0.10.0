// ue_wrap/engine/engine_emitter.cpp -- see ue_wrap/engine/engine_emitter.h.

#include "ue_wrap/engine/engine_emitter.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"  // the component's world place

namespace ue_wrap::engine {
namespace {

namespace R = reflection;
namespace P = profile;

// The CDO and function are the engine's own native class's: resolved once, and again if either has been freed.
void* g_cdo = nullptr;
void* g_fn = nullptr;

bool Resolve() {
    if (g_cdo && g_fn && R::IsLive(g_cdo) && R::IsLive(g_fn)) return true;
    g_cdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    g_fn = nullptr;
    if (g_cdo) {
        if (void* cls = R::ClassOf(g_cdo)) g_fn = R::FindFunction(cls, L"SpawnEmitterAtLocation");
    }
    return g_cdo && g_fn;
}

}  // namespace

bool SpawnEmitterAtLocation(void* worldContext, void* emitterTemplate, const FVector& location,
                            const FRotator& rotation, const FVector& scale) {
    if (!worldContext || !emitterTemplate || !Resolve()) return false;
    ParamFrame f(g_fn);
    f.Set<void*>(L"WorldContextObject", worldContext);
    f.Set<void*>(L"EmitterTemplate", emitterTemplate);
    f.SetRaw(L"Location", &location, sizeof(location));
    f.SetRaw(L"Rotation", &rotation, sizeof(rotation));
    f.SetRaw(L"Scale", &scale, sizeof(scale));
    f.Set<bool>(L"bAutoDestroy", true);
    f.Set<uint8_t>(L"PoolingMethod", 0);   // EPSCPoolMethod::None
    f.Set<bool>(L"bAutoActivateSystem", true);
    return Call(g_cdo, f);
}

bool ReadEmitterComponent(void* component, void*& emitterTemplate, FVector& location, FRotator& rotation) {
    if (!component || !R::IsLive(component)) return false;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(component), P::name::ParticleTemplateProp);
    if (off < 0) return false;
    emitterTemplate = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(component) + off);
    if (!emitterTemplate || !R::IsLive(emitterTemplate)) return false;
    if (!TryGetComponentLocation(component, location)) return false;
    rotation = GetComponentWorldRotation(component);
    return true;
}

}  // namespace ue_wrap::engine
