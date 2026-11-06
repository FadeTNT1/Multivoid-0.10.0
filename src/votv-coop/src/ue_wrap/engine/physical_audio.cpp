// ue_wrap/engine/physical_audio.cpp -- see ue_wrap/engine/physical_audio.h.

#include "ue_wrap/engine/physical_audio.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"

#include <cstdint>
#include <cwctype>
#include <string>
#include <vector>

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

void* AudioMaterialOf(void* physMat, void* worldCtx) {
    if (!physMat || !R::IsLive(physMat) || !worldCtx || !R::IsLive(worldCtx)) return nullptr;
    static bool sSaidLib = false, sSaidCall = false;
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
    return audioMat;
}


// ---- the particle half --------------------------------------------------------------------------------------------

bool ContainsNoCase(const std::wstring& hay, const wchar_t* needle) {
    std::wstring h = hay;
    for (wchar_t& c : h) c = static_cast<wchar_t>(::towlower(c));
    return h.find(needle) != std::wstring::npos;
}

// What the audio material's class carries, said once: each own member's name, its type and, for an object member, what
// it holds. The particle field of this build is not known by name, so the first lookup writes the list the next
// reading of the log needs.
void DumpFieldsOnce(void* audioMat) {
    static bool sDumped = false;
    if (sDumped) return;
    sDumped = true;
    void* cls = R::ClassOf(audioMat);
    const std::vector<R::StructFieldInfo> fields = R::EnumerateStructFields(cls);
    UE_LOGI("physical_audio: %ls has %zu own fields (the particle lookup picks an object field holding a particle "
            "system whose name says impact, damage or break):", R::ClassNameOf(audioMat).c_str(), fields.size());
    for (const R::StructFieldInfo& fi : fields) {
        int32_t off = -1;
        std::wstring type;
        R::PropertyOffsetAndClass(cls, fi.name.c_str(), off, type);
        std::wstring holds = L"-";
        if (type == L"ObjectProperty" && fi.size == static_cast<int32_t>(sizeof(void*))) {
            void* v = *reinterpret_cast<void**>(static_cast<uint8_t*>(audioMat) + fi.offset);
            if (v && R::IsLive(v)) holds = R::ClassNameOf(v) + L" " + R::ToString(R::NameOf(v));
        }
        UE_LOGI("physical_audio:   field %ls offset=%d size=%d type=%ls holds=%ls", fi.name.c_str(), fi.offset, fi.size,
                type.c_str(), holds.c_str());
    }
}

// The particle system a field of the audio material holds and whose name says `which`, else null.
void* ParticleOfAudioMaterial(void* audioMat, Cue which) {
    if (which == Cue::kSoft) return nullptr;
    DumpFieldsOnce(audioMat);
    void* cls = R::ClassOf(audioMat);
    for (const R::StructFieldInfo& fi : R::EnumerateStructFields(cls)) {
        const bool nameSays = which == Cue::kDestroyed
                                  ? (ContainsNoCase(fi.name, L"destroy") || ContainsNoCase(fi.name, L"break"))
                              : which == Cue::kDamaged ? ContainsNoCase(fi.name, L"damage")
                                                       : (ContainsNoCase(fi.name, L"impact") ||
                                                          ContainsNoCase(fi.name, L"hit"));
        if (!nameSays || fi.size != static_cast<int32_t>(sizeof(void*))) continue;
        int32_t off = -1;
        std::wstring type;
        if (!R::PropertyOffsetAndClass(cls, fi.name.c_str(), off, type) || type != L"ObjectProperty") continue;
        void* v = *reinterpret_cast<void**>(static_cast<uint8_t*>(audioMat) + fi.offset);
        if (v && R::IsLive(v) && R::ClassNameOf(v) == L"ParticleSystem") return v;
    }
    return nullptr;
}

}  // namespace

void* CueFromPhysicalMaterial(void* physMat, void* worldCtx, Cue which) {
    void* audioMat = AudioMaterialOf(physMat, worldCtx);
    if (!audioMat) return nullptr;

    static bool sSaidProp = false;
    static R::InstanceOffset sSoft(N::PhysAudioSoftProp);
    static R::InstanceOffset sImpact(N::PhysAudioImpactProp);
    static R::InstanceOffset sDamaged(N::PhysAudioDamagedProp);
    static R::InstanceOffset sDestroyed(N::PhysAudioDestroyedProp);
    R::InstanceOffset& off = which == Cue::kImpact    ? sImpact
                           : which == Cue::kDamaged   ? sDamaged
                           : which == Cue::kDestroyed ? sDestroyed
                                                      : sSoft;
    void* cue = ReadObjectProp(audioMat, off);
    // A material may leave its break cues empty; only the two every material carries are worth a warning.
    if (!cue && (which == Cue::kSoft || which == Cue::kImpact))
        LogOnce(sSaidProp, "audio material has no sound_soft / sound_impact property or it is empty");
    return cue;
}

void* ParticleFromPhysicalMaterial(void* physMat, void* worldCtx, Cue which) {
    void* audioMat = AudioMaterialOf(physMat, worldCtx);
    return audioMat ? ParticleOfAudioMaterial(audioMat, which) : nullptr;
}

namespace {

// The material a prop's physicsImpact component holds: the override when valid, else its own. Null for no component.
void* PhysMaterialOfProp(void* propActor) {
    if (!propActor || !R::IsLive(propActor)) return nullptr;
    static R::InstanceOffset sComp(N::PropPhysicsImpactProp);
    static R::InstanceOffset sOverride(N::PhysImpactMatOverrideProp);
    static R::InstanceOffset sMat(N::PhysImpactMatProp);
    void* comp = ReadObjectProp(propActor, sComp);
    if (!comp) return nullptr;
    void* physMat = ReadObjectProp(comp, sOverride);
    return physMat ? physMat : ReadObjectProp(comp, sMat);
}

}  // namespace

void* CueFromPropImpactComponent(void* propActor, Cue which) {
    return CueFromPhysicalMaterial(PhysMaterialOfProp(propActor), propActor, which);
}

void* ParticleFromPropImpactComponent(void* propActor, Cue which) {
    return ParticleFromPhysicalMaterial(PhysMaterialOfProp(propActor), propActor, which);
}

}  // namespace ue_wrap::physical_audio
