// ue_wrap/engine/physical_audio.cpp -- see ue_wrap/engine/physical_audio.h.

#include "ue_wrap/engine/physical_audio.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"

#include <cstdint>

namespace ue_wrap::physical_audio {
namespace {

namespace R = reflection;
namespace N = profile::name;

// A reflected object-pointer property of `obj`, or null: the offset is asked of the object's own
// class, so a class that does not declare it yields null rather than a stray read.
void* ReadObjectProp(void* obj, R::InstanceOffset& off) {
    if (!obj || !R::IsLive(obj)) return nullptr;
    const int32_t o = off.Of(obj);
    if (o < 0) return nullptr;
    void* v = *reinterpret_cast<void**>(static_cast<uint8_t*>(obj) + o);
    return (v && R::IsLive(v)) ? v : nullptr;
}

// A miss is said once per kind: this runs on every remote grab and land, and a cooked-name change
// would otherwise log each one.
void LogOnce(bool& said, const char* what) {
    if (said) return;
    said = true;
    UE_LOGW("physical_audio: %s -- grab / land cue skipped (logged once)", what);
}

}  // namespace

void* CueFromPhysicalMaterial(void* physMat, void* worldCtx, Cue which) {
    if (!physMat || !R::IsLive(physMat) || !worldCtx || !R::IsLive(worldCtx)) return nullptr;
    static bool sSaidLib = false, sSaidCall = false, sSaidProp = false;
    static void* sLibCdo = nullptr;
    static void* sFn = nullptr;
    // The cached CDO and UFunction can be freed by a class reload; re-resolve when dead.
    if (!sLibCdo || !sFn || !R::IsLive(sLibCdo) || !R::IsLive(sFn)) {
        sLibCdo = nullptr;
        sFn = nullptr;
        if (void* cls = R::FindClass(N::PhysAudioLibClass)) {
            sLibCdo = R::FindClassDefaultObject(N::PhysAudioLibClass);
            sFn = R::FindFunction(cls, N::GetPhysicalAsAudioFn);
        }
    }
    if (!sLibCdo || !sFn) {
        LogOnce(sSaidLib, "lib_C::getPhysicalAsAudio unresolved");
        return nullptr;
    }
    ParamFrame f(sFn);
    f.Set<void*>(N::GetPhysicalAsAudioMatParam, physMat);
    f.Set<void*>(N::GetPhysicalAsAudioWorldParam, worldCtx);
    if (!Call(sLibCdo, f)) {
        LogOnce(sSaidCall, "lib_C::getPhysicalAsAudio call failed");
        return nullptr;
    }
    void* audioMat = f.Get<void*>(N::GetPhysicalAsAudioOutParam);
    if (!audioMat || !R::IsLive(audioMat)) return nullptr;

    static R::InstanceOffset sSoft(N::PhysAudioSoftProp);
    static R::InstanceOffset sImpact(N::PhysAudioImpactProp);
    void* cue = ReadObjectProp(audioMat, which == Cue::kImpact ? sImpact : sSoft);
    if (!cue) LogOnce(sSaidProp, "audio material has no sound_soft / sound_impact property or it is empty");
    return cue;
}

void* CueFromPropImpactComponent(void* propActor, Cue which) {
    if (!propActor || !R::IsLive(propActor)) return nullptr;
    static R::InstanceOffset sComp(N::PropPhysicsImpactProp);
    static R::InstanceOffset sOverride(N::PhysImpactMatOverrideProp);
    static R::InstanceOffset sMat(N::PhysImpactMatProp);
    void* comp = ReadObjectProp(propActor, sComp);
    if (!comp) return nullptr;
    // Same pick the component's own audio lookup makes: the override when valid, else its material.
    void* physMat = ReadObjectProp(comp, sOverride);
    if (!physMat) physMat = ReadObjectProp(comp, sMat);
    return CueFromPhysicalMaterial(physMat, propActor, which);
}

}  // namespace ue_wrap::physical_audio
