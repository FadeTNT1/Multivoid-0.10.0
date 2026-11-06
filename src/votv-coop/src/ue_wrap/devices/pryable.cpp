// ue_wrap/devices/pryable.cpp -- see ue_wrap/devices/pryable.h.

#include "ue_wrap/devices/pryable.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_audio.h"
#include "ue_wrap/engine/engine_component.h"

#include <cstring>

namespace ue_wrap::pryable {
namespace {

namespace R = reflection;
namespace N = profile::name;

void* ChildClass() { return object_index::ClassByName(N::PryingCrowbarClass); }

// A member of pryingCrowbar_C, found by name on the class and asked again when a new world re-creates the class.
struct Member {
    const wchar_t* name;
    bool           isBool = false;
    CachedObjRef   cls;
    int32_t        off = -1;
    uint8_t        mask = 0xFF;
    bool           said = false;
};
Member g_pivot{N::PryPivotComp}, g_sk{N::PrySkeletalProp}, g_anim{N::PryAnimProp}, g_spark{N::PrySparkComp},
    g_strainA{N::PryStrainAudioA}, g_strainB{N::PryStrainAudioB}, g_ui{N::PryUiProp}, g_last{N::PryLastPlayerProp},
    g_alpha{N::PryAlphaProp}, g_stress{N::PryStressProp}, g_lives{N::PryLivesProp},
    g_strainSound{N::PryStrainSoundProp, true}, g_straining{N::PryStrainingProp, true},
    g_sparked{N::PrySparkedProp, true};

bool Resolve(Member& m) {
    void* cls = ChildClass();
    if (!cls) return false;
    if (m.cls.Is(cls)) return m.off >= 0;
    m.cls.Set(cls);
    m.off = -1;
    if (m.isBool) {
        int32_t off = -1;
        uint8_t mask = 0;
        if (R::FindBoolProperty(cls, m.name, off, mask)) { m.off = off; m.mask = mask; }
    } else {
        m.off = R::FindPropertyOffset(cls, m.name);
    }
    if (m.off < 0 && !m.said) {
        m.said = true;
        UE_LOGW("pryable: %ls.%ls did not resolve by name", N::PryingCrowbarClass, m.name);
    }
    return m.off >= 0;
}

uint8_t* At(void* obj, const Member& m) { return reinterpret_cast<uint8_t*>(obj) + m.off; }

void* ReadObj(void* child, Member& m) {
    if (!Resolve(m)) return nullptr;
    void* p = nullptr;
    std::memcpy(&p, At(child, m), sizeof(p));
    return p && R::IsLive(p) ? p : nullptr;
}
bool WriteObj(void* child, Member& m, void* value) {
    if (!Resolve(m)) return false;
    std::memcpy(At(child, m), &value, sizeof(value));
    return true;
}
bool ReadFloat(void* child, Member& m, float& out) {
    if (!Resolve(m)) return false;
    std::memcpy(&out, At(child, m), sizeof(out));
    return true;
}
bool WriteFloat(void* child, Member& m, float v) {
    if (!Resolve(m)) return false;
    std::memcpy(At(child, m), &v, sizeof(v));
    return true;
}
bool ReadBool(void* child, Member& m) { return Resolve(m) && (*At(child, m) & m.mask) != 0; }

// One property of an engine object read by name on its own class (a component's relative rotation, the AnimBP's
// angle): the class is known only from the instance, and the offset is kept for the class it was found on.
struct OwnMember {
    const wchar_t* name;
    void*          cls = nullptr;
    int32_t        off = -1;
};
OwnMember g_relRot{L"RelativeRotation"}, g_angle{N::PryAnimAngleProp};
int32_t OwnOffset(void* obj, OwnMember& m) {
    void* cls = obj ? R::ClassOf(obj) : nullptr;
    if (!cls) return -1;
    if (cls != m.cls) { m.cls = cls; m.off = R::FindPropertyOffset(cls, m.name); }
    return m.off;
}

bool SetPivotWorldLocation(void* pivot, const FVector& at) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(pivot), L"K2_SetWorldLocation");
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<FVector>(L"NewLocation", at) && Call(pivot, f);
}

bool SetPivotPitch(void* pivot, float pitch) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(pivot), N::SetRelativeRotationFn);
    if (!fn) return false;
    ParamFrame f(fn);
    FRotator r{};
    r.Pitch = pitch;
    return f.valid() && f.Set<FRotator>(L"NewRotation", r) && Call(pivot, f);
}

// The stick sound the begin plays at the spark, found by name once a world and kept while it lives.
void PlayStick(void* child) {
    static CachedObjRef s_cue;
    static void* s_att = nullptr;
    static bool s_said = false;
    void* cue = s_cue.Get();
    if (!cue) {
        cue = R::FindObject(N::PryStickCue, L"SoundCue");
        if (cue) s_cue.Set(cue);
    }
    if (!cue) {
        if (!s_said) { s_said = true; UE_LOGW("pryable: the stick sound '%ls' is not loaded -- a shown pry is silent", N::PryStickCue); }
        return;
    }
    if (!s_att) s_att = engine::SpawnSoundAttenuation(engine::SoundAttenuationConfig{});
    void* spark = ReadObj(child, g_spark);
    FVector at{};
    if (!(spark && engine::TryGetComponentLocation(spark, at)) && !engine::TryGetActorLocation(child, at)) return;
    engine::PlaySoundAtLocation(child, cue, at, s_att);
}

struct ForEachCtx { ChildFn fn; void* ctx; };
void Visit(void* ctx, void* obj, int32_t) {
    auto* c = static_cast<ForEachCtx*>(ctx);
    c->fn(c->ctx, obj);
}

}  // namespace

bool IsPryingCrowbar(void* obj) {
    if (!obj || !R::IsLive(obj)) return false;
    void* base = ChildClass();
    void* cls = R::ClassOf(obj);
    return base && cls && R::IsDescendantOfAny(cls, &base, 1);
}

size_t ForEachPryingCrowbar(ChildFn fn, void* ctx) {
    void* cls = ChildClass();
    if (!cls || !fn) return 0;
    ForEachCtx c{fn, ctx};
    return object_index::ForEachInstance(cls, &Visit, &c);
}

void* ParentOf(void* child) { return IsPryingCrowbar(child) ? engine::ParentActorOf(child) : nullptr; }

bool Read(void* child, State& out) {
    if (!IsPryingCrowbar(child)) return false;
    out = State{};
    void* pivot = ReadObj(child, g_pivot);
    if (!pivot || !ReadFloat(child, g_alpha, out.alpha) || !ReadFloat(child, g_stress, out.stress) ||
        !Resolve(g_lives))
        return false;
    engine::TryGetComponentLocation(pivot, out.pivot);
    const int32_t rr = OwnOffset(pivot, g_relRot);
    if (rr >= 0) {
        FRotator r{};
        std::memcpy(&r, reinterpret_cast<const uint8_t*>(pivot) + rr, sizeof(r));
        out.pitch = r.Pitch;
    }
    if (void* anim = ReadObj(child, g_anim)) {
        const int32_t ao = OwnOffset(anim, g_angle);
        if (ao >= 0) std::memcpy(&out.angle, reinterpret_cast<const uint8_t*>(anim) + ao, sizeof(float));
    }
    out.lives = *At(child, g_lives);
    out.stuck = ReadObj(child, g_sk) != nullptr;
    out.working = ReadObj(child, g_ui) != nullptr && ReadObj(child, g_last) != nullptr;
    out.straining = ReadBool(child, g_straining);
    out.strainSound = ReadBool(child, g_strainSound);
    out.sparked = ReadBool(child, g_sparked);
    return true;
}

bool ShowStuck(void* child, const FVector& pivotAt, uint8_t lives) {
    if (!IsPryingCrowbar(child) || !Resolve(g_lives)) return false;
    void* pivot = ReadObj(child, g_pivot);
    if (!pivot) return false;
    engine::SetActorTickEnabled(child, false);
    engine::SetActorHiddenInGame(child, false);
    engine::SetActorEnableCollision(child, false);
    *At(child, g_lives) = lives;
    WriteFloat(child, g_alpha, 0.f);
    WriteFloat(child, g_stress, 0.f);
    SetPivotWorldLocation(pivot, pivotAt);
    SetPivotPitch(pivot, 0.f);
    if (!ReadObj(child, g_sk)) component_calls::CallParamlessNamed(child, N::PryCreateSkeletalMeshFn);
    if (void* anim = ReadObj(child, g_anim)) {
        const int32_t ao = OwnOffset(anim, g_angle);
        const float zero = 0.f;
        if (ao >= 0) std::memcpy(reinterpret_cast<uint8_t*>(anim) + ao, &zero, sizeof(zero));
    }
    PlayStick(child);
    return ReadObj(child, g_sk) != nullptr;
}

bool ShowProgress(void* child, const State& now, const State& prev) {
    if (!IsPryingCrowbar(child)) return false;
    void* pivot = ReadObj(child, g_pivot);
    if (!pivot) return false;
    WriteFloat(child, g_alpha, now.alpha);
    WriteFloat(child, g_stress, now.stress);
    SetPivotPitch(pivot, now.pitch);
    if (void* anim = ReadObj(child, g_anim)) {
        const int32_t ao = OwnOffset(anim, g_angle);
        if (ao >= 0) std::memcpy(reinterpret_cast<uint8_t*>(anim) + ao, &now.angle, sizeof(float));
    }
    // The sender's mouseXY turns one strain sound on as its strain starts and the other at each flip of strainSound
    // while it lasts; the spark lights once as the strain passes three quarters.
    const bool strainEdge = now.straining && (!prev.straining || now.strainSound != prev.strainSound);
    if (strainEdge)
        if (void* a = ReadObj(child, now.strainSound ? g_strainB : g_strainA)) component_calls::Activate(a);
    if (now.sparked && !prev.sparked)
        if (void* s = ReadObj(child, g_spark)) component_calls::Activate(s);
    return true;
}

bool CallEnd(void* child, bool pryOpen, bool dropSelf, bool destroyed, void* lastPlayer, bool keepUi) {
    if (!IsPryingCrowbar(child)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(child), N::PryEndFn);
    if (!fn) return false;
    WriteObj(child, g_last, lastPlayer);
    if (!keepUi) WriteObj(child, g_ui, nullptr);
    ParamFrame f(fn);
    return f.valid() && f.Set<bool>(L"pryOpen", pryOpen) && f.Set<bool>(L"dropSelf", dropSelf) &&
           f.Set<bool>(L"destroyed", destroyed) && Call(child, f);
}

bool CallSnap(void* child, uint8_t lives) {
    if (!IsPryingCrowbar(child) || !Resolve(g_lives)) return false;
    *At(child, g_lives) = lives;
    WriteObj(child, g_last, nullptr);
    WriteObj(child, g_ui, nullptr);
    return component_calls::CallParamlessNamed(child, N::PrySnapFn);
}

EndParams EndParamsOf(void* endFunction) {
    EndParams p;
    if (!endFunction) return p;
    p.pryOpen = R::FindParamOffset(endFunction, L"pryOpen");
    p.dropSelf = R::FindParamOffset(endFunction, L"dropSelf");
    p.destroyed = R::FindParamOffset(endFunction, L"destroyed");
    return p;
}

}  // namespace ue_wrap::pryable
