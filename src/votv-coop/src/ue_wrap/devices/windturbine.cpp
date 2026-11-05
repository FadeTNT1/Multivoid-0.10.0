// ue_wrap/devices/windturbine.cpp -- see ue_wrap/devices/windturbine.h.

#include "ue_wrap/devices/windturbine.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>

namespace ue_wrap::windturbine {
namespace {

namespace R = reflection;

std::atomic<bool> g_resolved{false};

void*   g_cls = nullptr;            // windturbine_C UClass
int32_t g_offHeadRotation  = -1;    // float headRotation
int32_t g_offTargetRot     = -1;    // float targetRot
int32_t g_offRot           = -1;    // float rot (servo integrator)
int32_t g_offAlphaBlades   = -1;    // float alpha_blades
int32_t g_offBladesMomentum= -1;    // float bladesMomentum
int32_t g_offMult          = -1;    // float mult

// All six floats are WRITTEN by WriteState, so none has a numeric fallback (the 0.9.0n offsets are another
// field's bytes on a recooked class). The turbine is read and written only when every one reflects.
bool g_membersOk = false;

int32_t ResolveOff(void* cls, const wchar_t* name) {
    const int32_t off = R::FindPropertyOffset(cls, name);
    if (off < 0)
        UE_LOGW("windturbine: reflected %ls offset not found -- it is WRITTEN, so there is no numeric fallback: "
                "the turbine state is neither read nor written (this line prints once)", name);
    return off;
}

inline float ReadF(const void* obj, int32_t off) {
    return *reinterpret_cast<const float*>(reinterpret_cast<const char*>(obj) + off);
}
inline void WriteF(void* obj, int32_t off, float v) {
    *reinterpret_cast<float*>(reinterpret_cast<char*>(obj) + off) = v;
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;
    void* cls = R::FindClass(L"windturbine_C");
    if (!cls) return false;  // BP class not loaded yet -- retry

    g_offHeadRotation   = ResolveOff(cls, L"headRotation");
    g_offTargetRot      = ResolveOff(cls, L"targetRot");
    g_offRot            = ResolveOff(cls, L"rot");
    g_offAlphaBlades    = ResolveOff(cls, L"alpha_blades");
    g_offBladesMomentum = ResolveOff(cls, L"bladesMomentum");
    g_offMult           = ResolveOff(cls, L"mult");
    g_membersOk = g_offHeadRotation >= 0 && g_offTargetRot >= 0 && g_offRot >= 0 && g_offAlphaBlades >= 0 &&
                  g_offBladesMomentum >= 0 && g_offMult >= 0;
    g_cls = cls;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("windturbine: resolved windturbine_C=%p headRotation@0x%04X targetRot@0x%04X "
            "rot@0x%04X alpha_blades@0x%04X bladesMomentum@0x%04X mult@0x%04X",
            cls, g_offHeadRotation, g_offTargetRot, g_offRot, g_offAlphaBlades,
            g_offBladesMomentum, g_offMult);
    return true;
}

bool IsTurbine(void* obj) {
    if (!obj || !g_cls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_cls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

bool ReadState(void* turbine, State& out) {
    if (!turbine || !g_resolved.load(std::memory_order_acquire) || !g_membersOk) return false;
    out.headRotation   = ReadF(turbine, g_offHeadRotation);
    out.targetRot      = ReadF(turbine, g_offTargetRot);
    out.rot            = ReadF(turbine, g_offRot);
    out.alphaBlades    = ReadF(turbine, g_offAlphaBlades);
    out.bladesMomentum = ReadF(turbine, g_offBladesMomentum);
    out.mult           = ReadF(turbine, g_offMult);
    return true;
}

bool WriteState(void* turbine, const State& st) {
    if (!turbine || !g_resolved.load(std::memory_order_acquire) || !g_membersOk) return false;
    WriteF(turbine, g_offHeadRotation,   st.headRotation);
    WriteF(turbine, g_offTargetRot,      st.targetRot);
    WriteF(turbine, g_offRot,            st.rot);
    WriteF(turbine, g_offAlphaBlades,    st.alphaBlades);
    WriteF(turbine, g_offBladesMomentum, st.bladesMomentum);
    WriteF(turbine, g_offMult,           st.mult);
    return true;
}

}  // namespace ue_wrap::windturbine
