// ue_wrap/actors/toolbox.cpp -- see ue_wrap/actors/toolbox.h.

#include "ue_wrap/actors/toolbox.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/world/world_instances.h"

#include <cstring>

namespace ue_wrap::toolbox {
namespace {

namespace R = reflection;
namespace N = profile::name;

struct Member {
    const wchar_t* cls;
    const wchar_t* name;
    CachedObjRef   clsRef;
    int32_t        off = -1;
    bool           said = false;
};
Member g_fixing{N::ToolboxClass, N::ToolboxFixingActorProp}, g_fixes{N::ToolboxClass, N::ToolboxFixesProp},
    g_time{N::ToolboxClass, N::ToolboxTimeProp}, g_complete{N::ToolboxClass, N::ToolboxCompleteProp},
    g_damaged{N::BurstPipeClass, N::BurstPipeDamagedProp};

bool Resolve(Member& m) {
    void* cls = object_index::ClassByName(m.cls);
    if (!cls) return false;
    if (m.clsRef.Is(cls)) return m.off >= 0;
    m.clsRef.Set(cls);
    m.off = R::FindPropertyOffset(cls, m.name);
    if (m.off < 0 && !m.said) {
        m.said = true;
        UE_LOGW("toolbox: %ls.%ls did not resolve by name", m.cls, m.name);
    }
    return m.off >= 0;
}

template <class T>
bool Get(void* obj, Member& m, T& out) {
    if (!Resolve(m)) return false;
    std::memcpy(&out, reinterpret_cast<const uint8_t*>(obj) + m.off, sizeof(T));
    return true;
}
template <class T>
bool Put(void* obj, Member& m, const T& v) {
    if (!Resolve(m)) return false;
    std::memcpy(reinterpret_cast<uint8_t*>(obj) + m.off, &v, sizeof(T));
    return true;
}

bool IsOfClass(void* obj, const wchar_t* name) {
    if (!obj || !R::IsLive(obj)) return false;
    void* base = object_index::ClassByName(name);
    void* cls = R::ClassOf(obj);
    return base && cls && R::IsDescendantOfAny(cls, &base, 1);
}

}  // namespace

bool IsToolbox(void* obj) { return IsOfClass(obj, N::ToolboxClass); }

void* FixingActor(void* kit) {
    void* p = nullptr;
    if (!IsToolbox(kit) || !Get(kit, g_fixing, p)) return nullptr;
    return p && R::IsLive(p) ? p : nullptr;
}

bool ReadFix(void* kit, float& complete, float& timeSec, int32_t& fixes) {
    return IsToolbox(kit) && Get(kit, g_complete, complete) && Get(kit, g_time, timeSec) && Get(kit, g_fixes, fixes);
}

bool WriteComplete(void* kit, float complete) { return IsToolbox(kit) && Put(kit, g_complete, complete); }

bool CallCompleteSaw(void* kit) {
    return IsToolbox(kit) && component_calls::CallParamlessNamed(kit, N::ToolboxCompleteSawFn);
}

bool RunFixEnd(void* kit) {
    if (!IsToolbox(kit)) return false;
    Put(kit, g_complete, 1.0f);
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(kit), N::ReceiveTickFn);
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<float>(L"DeltaSeconds", 0.0f) && Call(kit, f);
}

void* FixSound() {
    void* cdo = R::FindClassDefaultObject(N::ToolboxClass);
    if (!cdo) return nullptr;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(cdo), N::ToolboxFixAudioProp);
    if (off < 0) return nullptr;
    void* snd = nullptr;
    std::memcpy(&snd, reinterpret_cast<const uint8_t*>(cdo) + off, sizeof(snd));
    return snd && R::IsLive(snd) ? snd : nullptr;
}

bool IsPipe(void* obj) { return IsOfClass(obj, N::BurstPipeClass); }

bool PipeDamaged(void* pipe, bool& damaged) {
    uint8_t byte = 0;
    if (!IsPipe(pipe) || !Resolve(g_damaged)) return false;
    // A bool property is a bit in a byte; the field offset the reflection gives is the byte's.
    uint8_t mask = 0;
    int32_t off = -1;
    if (!R::FindBoolProperty(R::ClassOf(pipe), N::BurstPipeDamagedProp, off, mask) || off < 0 || mask == 0) return false;
    std::memcpy(&byte, reinterpret_cast<const uint8_t*>(pipe) + off, 1);
    damaged = (byte & mask) != 0;
    return true;
}

bool PipeSetState(void* pipe, bool damaged) {
    void* fn = IsPipe(pipe) ? R::FindDispatchFunctionCached(R::ClassOf(pipe), N::BurstPipeStateFn) : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<bool>(N::BurstPipeStateParam, damaged) && Call(pipe, f);
}

bool PipeFixTime(void* pipe, float& timeSec) {
    void* fn = IsPipe(pipe) ? R::FindDispatchFunctionCached(R::ClassOf(pipe), N::BurstPipeFixTimeFn) : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid() || !Call(pipe, f)) return false;
    timeSec = f.Get<float>(L"time");
    return timeSec >= 0.f && timeSec < 3600.f;
}

bool PipeToolboxFix(void* pipe, void* kit, bool& fixed) {
    void* fn = IsPipe(pipe) ? R::FindDispatchFunctionCached(R::ClassOf(pipe), N::BurstPipeFixFn) : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(N::BurstPipeToolboxParam, kit);
    if (!Call(pipe, f)) return false;
    fixed = f.Get<bool>(L"return");
    return true;
}

size_t Pipes(void** out, size_t cap) {
    return static_cast<size_t>(world_instances::Find(N::BurstPipeClass, out, static_cast<int32_t>(cap)));
}

}  // namespace ue_wrap::toolbox
