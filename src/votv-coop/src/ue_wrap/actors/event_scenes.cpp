// ue_wrap/actors/event_scenes.cpp -- see ue_wrap/actors/event_scenes.h.

#include "ue_wrap/actors/event_scenes.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/active_events.h"
#include "ue_wrap/world/world_instances.h"
#include "ue_wrap/world/world_singleton.h"

#include <cmath>
#include <cstring>

namespace ue_wrap::event_scenes {
namespace {

namespace P  = profile;
namespace R  = reflection;
namespace E  = engine;
namespace CC = component_calls;

// The component (or any object member) `prop` of `actor`. A scene verb runs once per phase edge, so the lookup is
// not cached.
void* MemberObject(void* actor, const wchar_t* prop) {
    if (!actor || !R::IsLive(actor)) return nullptr;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), prop);
    if (off < 0) return nullptr;
    void* obj = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(actor) + off);
    return obj && R::IsLive(obj) ? obj : nullptr;
}

bool ReadBoolMember(void* actor, const wchar_t* prop, bool& out) {
    if (!actor || !R::IsLive(actor)) return false;
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(actor), prop, off, mask) || off < 0) return false;
    out = (*(static_cast<const uint8_t*>(actor) + off) & mask) != 0;
    return true;
}

bool WriteBoolMember(void* actor, const wchar_t* prop, bool value) {
    if (!actor || !R::IsLive(actor)) return false;
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(actor), prop, off, mask) || off < 0) return false;
    uint8_t* b = static_cast<uint8_t*>(actor) + off;
    *b = value ? static_cast<uint8_t>(*b | mask) : static_cast<uint8_t>(*b & ~mask);
    return true;
}

// The one-bool verb `fn` of `actor`, its parameter `param` set to `value`.
bool CallBoolVerb(void* actor, const wchar_t* fn, const wchar_t* param, bool value) {
    if (!actor || !R::IsLive(actor)) return false;
    void* f = R::FindDispatchFunctionCached(R::ClassOf(actor), fn);
    if (!f) return false;
    ParamFrame frame(f);
    if (!frame.valid()) return false;
    const uint8_t b = value ? 1 : 0;
    if (!frame.Set<uint8_t>(param, b)) return false;
    return Call(actor, frame);
}

// GameplayStatics.PlayWorldCameraShake(WorldContextObject, Shake, Epicenter, InnerRadius, OuterRadius, Falloff,
// bOrientShakeTowardsEpicenter) with the radii the obelisk's landing uses (5000 and 100000).
bool PlayShakeAt(void* actor, const wchar_t* shakeClass) {
    void* cls = R::FindClass(shakeClass);
    void* gsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    void* gsCls = R::FindClass(P::name::GameplayStaticsClass);
    void* fn = gsCls ? R::FindFunction(gsCls, L"PlayWorldCameraShake") : nullptr;
    FVector at{};
    if (!cls || !gsCdo || !fn || !E::TryGetActorLocation(actor, at)) return false;
    ParamFrame frame(fn);
    if (!frame.valid()) return false;
    frame.Set<void*>(L"WorldContextObject", actor);
    frame.Set<void*>(L"Shake", cls);
    frame.Set<FVector>(L"Epicenter", at);
    frame.Set<float>(L"InnerRadius", 5000.f);
    frame.Set<float>(L"OuterRadius", 100000.f);
    frame.Set<float>(L"Falloff", 1.f);
    return Call(gsCdo, frame);
}

}  // namespace

bool ReadBoolParam(void* function, const uint8_t* locals, const wchar_t* param, bool& out) {
    if (!function || !locals) return false;
    const int32_t off = R::FindParamOffset(function, param);
    if (off < 0) return false;
    out = locals[off] != 0;
    return true;
}

bool IsSoltomiaThrowNotify(void* function, const uint8_t* locals) {
    if (!function || !locals) return false;
    const int32_t off = R::FindParamOffset(function, P::name::SoltomiaNotifyNameParam);
    if (off < 0) return false;
    R::FName name{};
    std::memcpy(&name, locals + off, sizeof(name));
    return R::NameEquals(name, P::name::SoltomiaThrowNotify);
}

bool ReadSoltomiaActive(void* actor, bool& out) {
    return ReadBoolMember(actor, P::name::SoltomiaActiveProp, out);
}

bool SoltomiaReveal(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    bool ok = E::SetActorHiddenInGame(actor, false);
    if (void* squeak = MemberObject(actor, P::name::SoltomiaSqueakProp)) ok &= CC::SetActive(squeak, true, false);
    if (void* dust = MemberObject(actor, P::name::SoltomiaDustProp)) ok &= CC::Activate(dust);
    ok &= WriteBoolMember(actor, P::name::SoltomiaActiveProp, true);
    ok &= CC::CallParamlessNamed(actor, P::name::SoltomiaTalkingFn);
    return ok;
}

bool SoltomiaPlayMontage(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    return CC::CallParamlessNamed(actor, P::name::SoltomiaOpenedFn);
}

bool SoltomiaThrowPresentation(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    bool ok = true;
    if (void* mesh = MemberObject(actor, P::name::SoltomiaSpongeMeshProp))
        ok &= E::SetSceneComponentVisibility(mesh, false, false);
    if (void* squeak = MemberObject(actor, P::name::SoltomiaSqueakProp)) ok &= CC::SetActive(squeak, false, false);
    if (void* dust = MemberObject(actor, P::name::SoltomiaDustProp)) ok &= CC::SetActive(dust, false, false);
    return ok;
}

bool ObeliskLand(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    bool ok = true;
    if (void* land = MemberObject(actor, P::name::ObeliskLandAudioProp)) ok &= CC::Activate(land);
    else ok = false;
    if (!PlayShakeAt(actor, P::name::ObeliskLandShakeClass)) ok = false;
    if (void* mesh = MemberObject(actor, P::name::ObeliskMeshProp)) ok &= E::SetSceneComponentVisibility(mesh, false, false);
    if (void* dust = MemberObject(actor, P::name::ObeliskDustProp)) ok &= CC::SetActive(dust, false, false);
    return ok;
}

bool EndOwnEvent(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    active_events::Senders senders{};
    if (!active_events::EnsureResolved() || !active_events::ReadSenders(senders)) return false;
    bool registered = false;
    for (int32_t i = 0; i < senders.num && !registered; ++i) registered = senders.data[i] == actor;
    if (!registered) return true;   // nothing of its own to take back
    void* libCls = R::FindClass(P::name::LibClass);
    void* cdo = R::FindClassDefaultObject(P::name::LibClass);
    void* fn = libCls ? R::FindFunction(libCls, P::name::LibSetEventFn) : nullptr;
    if (!cdo || !fn) return false;
    ParamFrame frame(fn);
    if (!frame.valid()) return false;
    frame.Set<bool>(P::name::LibSetEventActiveParam, false);
    frame.Set<void*>(P::name::LibWorldContextParam, actor);
    return Call(cdo, frame);
}

bool ObeliskMarkFallen(void* actor) {
    return WriteBoolMember(actor, P::name::ObeliskFallenProp, true);
}

bool UfoShieldSetActive(void* actor, bool enabled) {
    return CallBoolVerb(actor, P::name::UfoShieldSetActiveFn, P::name::UfoShieldEnabledParam, enabled);
}

bool UfoShieldFlyAway(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    return CC::CallParamlessNamed(actor, P::name::UfoShieldFlyAwayFn);
}

bool AgravCloak(void* actor, bool forward) {
    bool ok = CallBoolVerb(actor, P::name::AgravCloakFn, P::name::AgravForwardParam, forward);
    if (void* hover = MemberObject(actor, P::name::AgravHoverProp)) ok &= CC::SetActive(hover, forward, false);
    return ok;
}

bool AgravPlayerLight(void* actor, bool on) {
    void* light = MemberObject(actor, P::name::AgravPlayerLightProp);
    if (!light) return !on;   // already destroyed (the game's own end) or never built: nothing to take back
    if (!on) return E::DestroyComponent(light, actor);   // runTrigger's own end: k2_DestroyComponent(pointlight)
    // runTrigger: pointlight.K2_AttachToComponent(playerCameraManager.RootComponent, None, KeepRelative x3, true),
    // then SetVisibility(true, false).
    void* camMgr = world_singleton::Find(P::name::PlayerCameraManagerClass);
    void* root = camMgr ? E::RootComponentOf(camMgr) : nullptr;
    void* fn = root ? R::FindDispatchFunctionCached(R::ClassOf(light), P::name::AttachToComponentFn) : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    const R::FName none{};
    f.Set<void*>(L"Parent", root);
    f.SetRaw(L"SocketName", &none, sizeof(none));
    f.Set<uint8_t>(L"LocationRule", uint8_t{0});   // EAttachmentRule::KeepRelative
    f.Set<uint8_t>(L"RotationRule", uint8_t{0});
    f.Set<uint8_t>(L"ScaleRule", uint8_t{0});
    f.Set<bool>(L"bWeldSimulatedBodies", true);
    bool ok = Call(light, f);
    // SetComponentVisible also clears hidden-in-game: the host's census read this light as hidden mid-event.
    ok &= E::SetComponentVisible(light, true, false);
    return ok;
}

size_t AgravGatheredComponents(void* actor, std::vector<void*>& out) {
    if (!actor || !R::IsLive(actor)) return 0;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), P::name::AgravCompsProp);
    if (off < 0) return 0;
    const auto* arr = reinterpret_cast<const field_io::TArrayView*>(static_cast<const uint8_t*>(actor) + off);
    constexpr int32_t kMaxComps = 4096;   // the game's gather is a sphere of 10000 cm: far fewer in a real base
    if (!arr->data || arr->num <= 0 || arr->num > arr->max || arr->num > kMaxComps) return 0;
    size_t added = 0;
    for (int32_t i = 0; i < arr->num; ++i) {
        void* comp = reinterpret_cast<void* const*>(arr->data)[i];
        if (!comp || !R::IsLive(comp)) continue;
        out.push_back(comp);
        ++added;
    }
    return added;
}

bool MidasProgress(void* actor, float& out) {
    if (!actor || !R::IsLive(actor)) return false;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), P::name::MidasUfoProgressProp);
    if (off < 0) return false;
    out = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(actor) + off);
    return std::isfinite(out);
}

bool MidasRun(void* actor, float fraction) {
    if (!CallBoolVerb(actor, P::name::MidasUfoSetActiveFn, P::name::MidasUfoActiveParam, true)) return false;
    if (fraction > 0.f) {
        // setActive reset `a` to 0 and placed the UFO at the spline's start; its tick eases it from there to where `a`
        // says, so a joiner's UFO flies in to the host's place.
        const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), P::name::MidasUfoProgressProp);
        if (off < 0) return false;
        *reinterpret_cast<float*>(static_cast<uint8_t*>(actor) + off) = fraction;
    }
    return true;
}

bool UfoDropperDeploy(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    return CC::CallParamlessNamed(actor, P::name::UfoDropperDeployFn);
}

bool ReadMorningUfoFly(void* actor, bool& out) {
    return ReadBoolMember(actor, P::name::MorningUfoFlyProp, out);
}

bool MorningUfoFly(void* actor) {
    if (!WriteBoolMember(actor, P::name::MorningUfoFlyProp, true)) return false;
    return E::SetActorHiddenInGame(actor, false);
}

bool ReadVentCrawlerBox(void* crawler, FVector& center, FVector& extent, FRotator& rotation) {
    void* box = MemberObject(crawler, P::name::VentCrawlerBoxProp);
    if (!box) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(box), P::name::BoxGetScaledExtentFn);
    if (!fn) return false;
    ParamFrame frame(fn);
    if (!frame.valid() || !Call(box, frame)) return false;
    if (!frame.GetRaw(L"ReturnValue", &extent, sizeof(extent))) return false;
    if (!E::TryGetComponentLocation(box, center)) return false;
    rotation = E::GetComponentWorldRotation(box);
    return true;
}

bool CallVentCrawlerOverlap(void* crawler, void* other) {
    void* box = MemberObject(crawler, P::name::VentCrawlerBoxProp);
    if (!box || !other || !R::IsLive(other)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(crawler), P::name::VentCrawlerBoxOverlapFn);
    if (!fn) return false;
    ParamFrame frame(fn);
    if (!frame.valid()) return false;
    frame.Set<void*>(L"OverlappedComponent", box);
    frame.Set<void*>(L"OtherActor", other);
    return Call(crawler, frame);
}

std::wstring ShortName(void* actor) {
    if (!actor || !R::IsLive(actor)) return std::wstring();
    return R::ToString(R::NameOf(actor));
}

void* FindLevelActor(const wchar_t* className, const std::wstring& name) {
    if (!className || name.empty()) return nullptr;
    void* found[16] = {};
    const int32_t n = world_instances::Find(className, found, 16);
    for (int32_t i = 0; i < n && i < 16; ++i)
        if (found[i] && R::NameEquals(R::NameOf(found[i]), name.c_str())) return found[i];
    return nullptr;
}

void* FindFirstLevelActor(const wchar_t* className) {
    return className ? world_singleton::Find(className) : nullptr;
}

}  // namespace ue_wrap::event_scenes
