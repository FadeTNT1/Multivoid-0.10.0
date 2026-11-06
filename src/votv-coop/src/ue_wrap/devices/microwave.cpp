// ue_wrap/devices/microwave.cpp -- see ue_wrap/devices/microwave.h.

#include "ue_wrap/devices/microwave.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"

#include <cmath>
#include <cstring>
#include <string>

namespace ue_wrap::microwave {
namespace {

namespace R = reflection;
namespace P = profile;

// A member found on prop_microwave_C (or, for `cordPowered`, on the prop_corded_C it inherits from, which the property
// lookup climbs to) by name and type; asked again when a new world re-creates the class.
struct Member {
    const wchar_t* name;
    const wchar_t* type;
    CachedObjRef   cls;
    int32_t        off = -1;
    uint8_t        mask = 0;
    bool           ok = false;
    bool           said = false;
};
Member g_active{P::name::MicrowaveActiveProp, L"BoolProperty", {}};
Member g_opened{P::name::MicrowaveOpenedProp, L"BoolProperty", {}};
Member g_powered{P::name::CordedPoweredProp, L"BoolProperty", {}};
Member g_timeMode{P::name::MicrowaveTimeModeProp, L"IntProperty", {}};
Member g_timer{P::name::MicrowaveTimerProp, L"FloatProperty", {}};
Member g_time{P::name::MicrowaveTimeProp, L"FloatProperty", {}};
Member g_loop{P::name::MicrowaveLoopComp, L"ObjectProperty", {}};
Member g_ding{P::name::MicrowaveDingComp, L"ObjectProperty", {}};
Member g_light{P::name::MicrowaveLightComp, L"ObjectProperty", {}};
Member g_spin{P::name::MicrowaveSpinComp, L"ObjectProperty", {}};

void* MicrowaveClass() { return object_index::ClassByName(P::name::MicrowaveClass); }

bool Resolve(Member& m) {
    void* cls = MicrowaveClass();
    if (!cls) return false;
    if (m.cls.Is(cls)) return m.ok;
    m.cls.Set(cls);
    m.ok = false;
    if (std::wcscmp(m.type, L"BoolProperty") == 0) {
        int32_t off = -1;
        uint8_t mask = 0;
        m.ok = R::FindBoolProperty(cls, m.name, off, mask) && off >= 0 && mask != 0;
        m.off = off;
        m.mask = mask;
    } else {
        int32_t off = -1;
        std::wstring found;
        m.ok = R::PropertyOffsetAndClass(cls, m.name, off, found) && off >= 0 && found == m.type;
        m.off = off;
    }
    if (!m.ok && !m.said) {
        m.said = true;
        UE_LOGW("microwave: %ls.%ls (%ls) did not resolve by name and type", P::name::MicrowaveClass, m.name, m.type);
    }
    return m.ok;
}

uint8_t* At(void* obj, const Member& m) { return reinterpret_cast<uint8_t*>(obj) + m.off; }

bool ReadBool(void* obj, Member& m, bool& out) {
    if (!Resolve(m)) return false;
    out = (*At(obj, m) & m.mask) != 0;
    return true;
}

bool WriteBool(void* obj, Member& m, bool v) {
    if (!Resolve(m)) return false;
    uint8_t& b = *At(obj, m);
    b = v ? static_cast<uint8_t>(b | m.mask) : static_cast<uint8_t>(b & ~m.mask);
    return true;
}

template <class T>
bool ReadAt(void* obj, Member& m, T& out) {
    if (!Resolve(m)) return false;
    std::memcpy(&out, At(obj, m), sizeof(T));
    return true;
}

template <class T>
bool WriteAt(void* obj, Member& m, const T& v) {
    if (!Resolve(m)) return false;
    std::memcpy(At(obj, m), &v, sizeof(T));
    return true;
}

void* Component(void* mw, Member& m) {
    void* comp = nullptr;
    if (!ReadAt(mw, m, comp)) return nullptr;
    return comp && R::IsLive(comp) ? comp : nullptr;
}

bool SetVisible(void* comp, bool visible) {
    if (!comp) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(comp), P::name::SetVisibilityFn);
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<bool>(L"bNewVisibility", visible) && f.Set<bool>(L"bPropagateToChildren", false) &&
           Call(comp, f);
}

}  // namespace

bool IsMicrowave(void* obj) {
    if (!obj || !R::IsLive(obj)) return false;
    void* base = MicrowaveClass();
    void* cls = R::ClassOf(obj);
    return base && cls && R::IsDescendantOfAny(cls, &base, 1);
}

bool ReadState(void* mw, State& out) {
    if (!IsMicrowave(mw)) return false;
    State s;
    if (!ReadBool(mw, g_active, s.active) || !ReadBool(mw, g_opened, s.opened) ||
        !ReadAt(mw, g_timeMode, s.timeMode) || !ReadAt(mw, g_timer, s.timer) || !ReadAt(mw, g_time, s.time))
        return false;
    if (!ReadBool(mw, g_powered, s.powered)) s.powered = true;   // an uncorded build: the loop's own test is absent
    if (!std::isfinite(s.timer) || !std::isfinite(s.time)) return false;
    out = s;
    return true;
}

bool WriteMode(void* mw, int32_t timeMode, float timer) {
    return IsMicrowave(mw) && WriteAt(mw, g_timeMode, timeMode) && WriteAt(mw, g_timer, timer);
}

bool WriteTime(void* mw, float time) { return IsMicrowave(mw) && WriteAt(mw, g_time, time); }

bool ShowRunning(void* mw, float remaining) {
    if (!IsMicrowave(mw) || !WriteBool(mw, g_active, true) || !WriteAt(mw, g_time, remaining)) return false;
    SetVisible(Component(mw, g_light), true);
    if (void* loop = Component(mw, g_loop)) component_calls::Activate(loop);
    return true;
}

bool ShowStopped(void* mw, bool ding) {
    if (!IsMicrowave(mw)) return false;
    if (ding)
        if (void* d = Component(mw, g_ding)) component_calls::Activate(d);
    WriteAt(mw, g_time, 0.f);
    WriteBool(mw, g_active, false);
    SetVisible(Component(mw, g_light), false);
    if (void* loop = Component(mw, g_loop)) component_calls::SetActive(loop, false, false);
    return true;
}

bool TurnPlate(void* mw, float yawDegrees) {
    void* spin = IsMicrowave(mw) ? Component(mw, g_spin) : nullptr;
    if (!spin) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(spin), P::name::AddLocalRotationFn);
    if (!fn) return false;
    ParamFrame f(fn);
    FRotator delta{};
    delta.Yaw = yawDegrees;
    return f.valid() && f.Set<FRotator>(L"DeltaRotation", delta) && f.Set<bool>(L"bSweep", false) &&
           f.Set<bool>(L"bTeleport", false) && Call(spin, f);
}

}  // namespace ue_wrap::microwave
