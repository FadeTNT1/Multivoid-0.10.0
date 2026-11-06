// ue_wrap/actors/hacksaw.cpp -- see ue_wrap/actors/hacksaw.h.

#include "ue_wrap/actors/hacksaw.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine_component.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace ue_wrap::hacksaw {
namespace {

namespace R = reflection;
namespace N = profile::name;

void* SawClass() { return object_index::ClassByName(N::HacksawClass); }

struct Member {
    const wchar_t* name;
    CachedObjRef   cls;
    int32_t        off = -1;
    bool           said = false;
};
Member g_prop{N::HacksawSawingProp}, g_complete{N::HacksawCompleteProp}, g_time{N::HacksawTimeProp},
    g_amount{N::HacksawAmountProp};

bool Resolve(Member& m) {
    void* cls = SawClass();
    if (!cls) return false;
    if (m.cls.Is(cls)) return m.off >= 0;
    m.cls.Set(cls);
    m.off = R::FindPropertyOffset(cls, m.name);
    if (m.off < 0 && !m.said) {
        m.said = true;
        UE_LOGW("hacksaw: %ls.%ls did not resolve by name", N::HacksawClass, m.name);
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

// An object property of an engine object, read by name on the object's own class.
void* ObjectProperty(void* obj, const wchar_t* name) {
    if (!obj) return nullptr;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), name);
    if (off < 0) return nullptr;
    void* p = nullptr;
    std::memcpy(&p, reinterpret_cast<const uint8_t*>(obj) + off, sizeof(p));
    return p && R::IsLive(p) ? p : nullptr;
}

// A function's return value, the call taking no parameter but `intArg` at its first when given.
void* CallReturningObject(void* obj, const wchar_t* fnName, const int32_t* intArg) {
    void* fn = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fnName) : nullptr;
    if (!fn) return nullptr;
    const std::vector<R::ParamInfo> params = R::FunctionParams(fn);
    ParamFrame f(fn);
    if (!f.valid()) return nullptr;
    if (intArg) {
        if (params.empty() || params[0].size != static_cast<int32_t>(sizeof(int32_t))) return nullptr;
        f.Set<int32_t>(params[0].name.c_str(), *intArg);
    }
    if (!Call(obj, f)) return nullptr;
    void* out = f.Get<void*>(L"ReturnValue");
    return out && R::IsLive(out) ? out : nullptr;
}

bool MeshVolume(void* mesh, float& volume) {
    void* cdo = R::FindClassDefaultObject(N::MeshVolumeLibClass);
    void* fn = cdo ? R::FindDispatchFunctionCached(R::ClassOf(cdo), N::MeshVolumeFn) : nullptr;
    if (!fn) return false;
    const std::vector<R::ParamInfo> params = R::FunctionParams(fn);
    if (params.size() < 2 || params[0].size != static_cast<int32_t>(sizeof(void*)) ||
        params[1].size != static_cast<int32_t>(sizeof(float)))
        return false;
    ParamFrame f(fn);
    if (!f.valid() || !f.Set<void*>(params[0].name.c_str(), mesh) || !Call(cdo, f)) return false;
    volume = f.Get<float>(params[1].name.c_str());
    return std::isfinite(volume) && volume >= 0.f;
}

bool BoolReturn(void* obj, const wchar_t* fnName, bool& out) {
    void* fn = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fnName) : nullptr;
    if (!fn) return false;
    const std::vector<R::ParamInfo> params = R::FunctionParams(fn);
    if (params.size() != 1 || params[0].size != 1) return false;
    ParamFrame f(fn);
    if (!f.valid() || !Call(obj, f)) return false;
    out = f.Get<uint8_t>(params[0].name.c_str()) != 0;
    return true;
}

}  // namespace

bool IsHacksaw(void* obj) {
    if (!obj || !R::IsLive(obj)) return false;
    void* base = SawClass();
    void* cls = R::ClassOf(obj);
    return base && cls && R::IsDescendantOfAny(cls, &base, 1);
}

void* SawingProp(void* saw) {
    void* p = nullptr;
    if (!IsHacksaw(saw) || !Get(saw, g_prop, p)) return nullptr;
    return p && R::IsLive(p) ? p : nullptr;
}

bool ReadCut(void* saw, float& complete, float& timeSec) {
    return IsHacksaw(saw) && Get(saw, g_complete, complete) && Get(saw, g_time, timeSec);
}

bool WriteComplete(void* saw, float complete) { return IsHacksaw(saw) && Put(saw, g_complete, complete); }

bool CallCompleteSaw(void* saw) {
    return IsHacksaw(saw) && component_calls::CallParamlessNamed(saw, N::HacksawCompleteSawFn);
}

bool MeasureCut(void* target, float& timeSec, int32_t& amount) {
    void* comp = engine::GetStaticMeshComponent(target);
    void* mesh = ObjectProperty(comp, L"StaticMesh");
    float volume = 0.f;
    if (!mesh || !MeshVolume(mesh, volume)) return false;
    const int32_t slot0 = 0;
    void* material = CallReturningObject(mesh, L"GetMaterial", &slot0);
    void* phys = CallReturningObject(material, L"GetPhysicalMaterial", nullptr);
    const int32_t dOff = phys ? R::FindPropertyOffset(R::ClassOf(phys), L"Density") : -1;
    if (dOff < 0) return false;
    float density = 0.f;
    std::memcpy(&density, reinterpret_cast<const uint8_t*>(phys) + dOff, sizeof(density));
    const float units = volume / N::HacksawVolumeDivisor;
    timeSec = units * density;
    const float rounded = std::round(units);
    amount = rounded < 1.f ? 1 : static_cast<int32_t>(rounded);
    return std::isfinite(timeSec);
}

bool TargetSawable(void* target) {
    bool notSawable = true;
    return target && R::IsLive(target) && BoolReturn(target, N::IsNotSawableFn, notSawable) && !notSawable;
}

bool RunCutEnd(void* saw, void* target, int32_t amount) {
    if (!IsHacksaw(saw) || !target || !R::IsLive(target)) return false;
    if (!Put(saw, g_prop, target)) return false;
    bool sawable = false;
    if (!BoolReturn(saw, N::HacksawProcessMatFn, sawable) || !sawable) {
        Put<void*>(saw, g_prop, nullptr);
        return false;
    }
    Put(saw, g_amount, amount);
    Put(saw, g_time, 1.0f);
    Put(saw, g_complete, 1.0f);
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(saw), N::ReceiveTickFn);
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<float>(L"DeltaSeconds", 0.0f) && Call(saw, f);
}

}  // namespace ue_wrap::hacksaw
