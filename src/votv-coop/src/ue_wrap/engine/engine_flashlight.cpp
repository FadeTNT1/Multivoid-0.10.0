// ue_wrap/engine/engine_flashlight.cpp -- see ue_wrap/engine/engine_flashlight.h. The setters
// dispatch the engine's own light UFunctions, which mark the render state dirty where a field write
// would leave the proxy stale; the caches are file-private, resolved once per process.

#include "ue_wrap/engine/engine.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflected_offset.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace ue_wrap::engine {
namespace {

namespace P = profile;
namespace R = reflection;

// The cached UFunctions for the light and cone setters below, resolved on the first
// successful call: one resolve per process, and gameplay code stays reflection-free.
void* g_setLightIntensityFn      = nullptr;
void* g_setSceneVisibilityFn     = nullptr;
void* g_setSpotOuterConeAngleFn  = nullptr;
void* g_setSpotInnerConeAngleFn  = nullptr;

void* ResolveLightIntensityFn() {
    if (g_setLightIntensityFn) return g_setLightIntensityFn;
    void* cls = R::FindClass(L"LightComponent");
    if (!cls) return nullptr;
    g_setLightIntensityFn = R::FindFunction(cls, P::name::SetIntensityFn);
    return g_setLightIntensityFn;
}
void* ResolveSceneVisibilityFn() {
    if (g_setSceneVisibilityFn) return g_setSceneVisibilityFn;
    void* cls = R::FindClass(P::name::SceneComponentClass);
    if (!cls) return nullptr;
    g_setSceneVisibilityFn = R::FindFunction(cls, P::name::SetVisibilityFn);
    return g_setSceneVisibilityFn;
}
// The light fields 0.10.0's flashlight properties set, read off the engine's own classes once.
struct LightFields {
    bool    resolved = false;
    int32_t attenuationRadius = -1;  // ULocalLightComponent
    int32_t lightColor = -1;         // ULightComponent (FColor)
    int32_t iesTexture = -1;         // ULightComponent (UTextureLightProfile*)
    int32_t maxDrawDistance = -1;    // ULightComponent (float)
};
const LightFields& Fields() {
    static LightFields f;
    if (f.resolved) return f;
    void* cls = R::FindClass(P::name::SpotLightComponentClass);
    if (!cls) return f;
    f.attenuationRadius = R::FindPropertyOffset(cls, P::name::LightAttenuationRadiusProp);
    f.lightColor = R::FindPropertyOffset(cls, P::name::LightColorProp);
    f.iesTexture = R::FindPropertyOffset(cls, P::name::LightIESTextureProp);
    f.maxDrawDistance = R::FindPropertyOffset(cls, P::name::LightMaxDrawDistanceProp);
    f.resolved = true;
    return f;
}

void* g_setAttenuationRadiusFn = nullptr;
void* g_setLightColorFn = nullptr;
void* g_setIESTextureFn = nullptr;
void* g_setCastShadowsFn = nullptr;

void* ResolveOn(void*& cache, const wchar_t* className, const wchar_t* fn) {
    if (cache) return cache;
    if (void* cls = R::FindClass(className)) cache = R::FindFunction(cls, fn);
    return cache;
}

// The sRGB decode SetLightColor's bSRGB encode inverts (FLinearColor::ToFColor(true)).
float SrgbToLinear(uint8_t c) {
    const float v = static_cast<float>(c) / 255.f;
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

void ResolveSpotConeFns() {
    if (g_setSpotOuterConeAngleFn && g_setSpotInnerConeAngleFn) return;
    void* cls = R::FindClass(P::name::SpotLightComponentClass);
    if (!cls) return;
    if (!g_setSpotOuterConeAngleFn) g_setSpotOuterConeAngleFn = R::FindFunction(cls, P::name::SetOuterConeAngleFn);
    if (!g_setSpotInnerConeAngleFn) g_setSpotInnerConeAngleFn = R::FindFunction(cls, P::name::SetInnerConeAngleFn);
}

}  // namespace

void* GetMainPlayerLightR(void* mainPlayer) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return nullptr;
    void* light = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(mainPlayer) + P::off::AmainPlayer_light_R);
    if (!light || !R::IsLive(light)) return nullptr;
    return light;
}

bool ReadFlashlightSnapshot(void* light, FlashlightSnapshot& out) {
    if (!light || !R::IsLive(light)) return false;
    auto* base = reinterpret_cast<uint8_t*>(light);
    const LightFields& lf = Fields();
    out.attenuationRadius = lf.attenuationRadius >= 0
                                ? *reinterpret_cast<float*>(base + lf.attenuationRadius) : -1.f;
    out.color = {};
    if (lf.lightColor >= 0) std::memcpy(&out.color, base + lf.lightColor, sizeof(out.color));
    out.iesTexture = lf.iesTexture >= 0 ? *reinterpret_cast<void**>(base + lf.iesTexture) : nullptr;
    out.intensity       = *reinterpret_cast<float*>(base + P::off::ULightComponentBase_Intensity);
    out.outerConeAngle  = *reinterpret_cast<float*>(base + P::off::USpotLightComponent_OuterConeAngle);
    out.innerConeAngle  = *reinterpret_cast<float*>(base + P::off::USpotLightComponent_InnerConeAngle);
    const uint8_t flags = *reinterpret_cast<uint8_t*>(base + P::off::USceneComponent_VisFlagsByte);
    out.visible         = (flags & 0x10) != 0;
    const uint8_t hidden = *reinterpret_cast<uint8_t*>(base + P::off::USceneComponent_HiddenFlagsByte);
    out.hiddenInGame    = (hidden & 0x04) != 0;
    return true;
}

bool ReadMainPlayerFlashlightState(void* mainPlayer, MainPlayerFlashlightState& out) {
    out = {};
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    auto* base = reinterpret_cast<uint8_t*>(mainPlayer);
    out.flashlight      = *reinterpret_cast<bool*>(base + P::off::AmainPlayer_flashlight);
    const int32_t equippedOff = ue_wrap::reflected_offset::MainPlayer_equipped_flashlight();
    out.equipped        = equippedOff >= 0 && *reinterpret_cast<bool*>(base + equippedOff);
    out.mode            = *reinterpret_cast<uint8_t*>(base + P::off::AmainPlayer_flashlightMode);
    return true;
}

bool WriteMainPlayerFlashlight(void* mainPlayer, bool newState) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    auto* base = reinterpret_cast<uint8_t*>(mainPlayer);
    *reinterpret_cast<bool*>(base + P::off::AmainPlayer_flashlight) = newState;
    return true;
}

bool SetLightIntensity(void* light, float newIntensity) {
    if (!light || !R::IsLive(light)) return false;
    void* fn = ResolveLightIntensityFn();
    if (!fn) return false;
    ParamFrame f(fn);
    f.Set<float>(L"NewIntensity", newIntensity);
    return Call(light, f);
}

bool SetSceneComponentVisibility(void* sceneComponent, bool newVisibility, bool propagateToChildren) {
    if (!sceneComponent || !R::IsLive(sceneComponent)) return false;
    void* fn = ResolveSceneVisibilityFn();
    if (!fn) return false;
    ParamFrame f(fn);
    f.Set<bool>(L"bNewVisibility", newVisibility);
    f.Set<bool>(L"bPropagateToChildren", propagateToChildren);
    return Call(sceneComponent, f);
}

bool SetSpotLightOuterConeAngle(void* spotLight, float newAngle) {
    if (!spotLight || !R::IsLive(spotLight)) return false;
    ResolveSpotConeFns();
    if (!g_setSpotOuterConeAngleFn) return false;
    ParamFrame f(g_setSpotOuterConeAngleFn);
    f.Set<float>(L"NewOuterConeAngle", newAngle);
    return Call(spotLight, f);
}

bool SetSpotLightInnerConeAngle(void* spotLight, float newAngle) {
    if (!spotLight || !R::IsLive(spotLight)) return false;
    ResolveSpotConeFns();
    if (!g_setSpotInnerConeAngleFn) return false;
    ParamFrame f(g_setSpotInnerConeAngleFn);
    f.Set<float>(L"NewInnerConeAngle", newAngle);
    return Call(spotLight, f);
}

bool SetLightAttenuationRadius(void* light, float radius) {
    if (!light || !R::IsLive(light)) return false;
    void* fn = ResolveOn(g_setAttenuationRadiusFn, P::name::LocalLightComponentClass,
                         P::name::SetAttenuationRadiusFn);
    if (!fn) return false;
    ParamFrame f(fn);
    f.Set<float>(L"NewRadius", radius);
    return Call(light, f);
}

bool SetLightColor(void* light, FColor color) {
    if (!light || !R::IsLive(light)) return false;
    void* fn = ResolveOn(g_setLightColorFn, P::name::LightComponentClass, P::name::SetLightColorFn);
    if (!fn) return false;
    const FLinearColor lin{SrgbToLinear(color.R), SrgbToLinear(color.G), SrgbToLinear(color.B),
                           static_cast<float>(color.A) / 255.f};
    ParamFrame f(fn);
    f.Set<FLinearColor>(L"NewLightColor", lin);
    f.Set<bool>(L"bSRGB", true);
    return Call(light, f);
}

bool SetLightIESTexture(void* light, void* textureLightProfile) {
    if (!light || !R::IsLive(light)) return false;
    void* fn = ResolveOn(g_setIESTextureFn, P::name::LightComponentClass, P::name::SetIESTextureFn);
    if (!fn) return false;
    ParamFrame f(fn);
    f.Set<void*>(L"NewValue", textureLightProfile);
    return Call(light, f);
}

bool SetLightCastShadows(void* light, bool castShadows) {
    if (!light || !R::IsLive(light)) return false;
    void* fn = ResolveOn(g_setCastShadowsFn, P::name::LightComponentBaseClass, P::name::SetCastShadowsFn);
    if (!fn) return false;
    ParamFrame f(fn);
    f.Set<bool>(L"bNewValue", castShadows);
    return Call(light, f);
}

float ReadLightMaxDrawDistance(void* light) {
    if (!light || !R::IsLive(light)) return -1.f;
    const LightFields& lf = Fields();
    if (lf.maxDrawDistance < 0) return -1.f;
    return *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(light) + lf.maxDrawDistance);
}

bool WriteLightMaxDrawDistance(void* light, float distance) {
    if (!light || !R::IsLive(light)) return false;
    const LightFields& lf = Fields();
    if (lf.maxDrawDistance < 0) return false;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(light) + lf.maxDrawDistance) = distance;
    return true;
}

float ReadMainPlayerArmLength(void* mainPlayer) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return -1.f;
    static int32_t s_off = -2;   // -2 unresolved, -1 absent
    if (s_off == -2) {
        void* cls = R::FindClass(P::name::MainPlayerClass);
        if (!cls) return -1.f;
        s_off = R::FindPropertyOffset(cls, P::name::MainPlayerArmLengthProp);
    }
    if (s_off < 0) return -1.f;
    return *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(mainPlayer) + s_off);
}

}  // namespace ue_wrap::engine
