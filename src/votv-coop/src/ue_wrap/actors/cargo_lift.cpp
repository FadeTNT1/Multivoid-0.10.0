// ue_wrap/actors/cargo_lift.cpp -- see ue_wrap/actors/cargo_lift.h.

#include "ue_wrap/actors/cargo_lift.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/world/world_instances.h"

#include <cstring>

namespace ue_wrap::cargo_lift {
namespace {

namespace R = ue_wrap::reflection;
namespace N = ue_wrap::profile::name;

// cargolift_C as this world holds it, by slot and serial (a Blueprint class can get a new object in a new world).
ue_wrap::CachedObjRef g_liftCls;

void* LiftClass() {
    if (void* c = g_liftCls.Get()) return c;
    void* c = ue_wrap::object_index::ClassByName(N::CargoLiftClass);
    if (c) g_liftCls.Set(c);
    return c;
}

// The lift's field places, resolved per class object.
struct Fields {
    void*   cls = nullptr;
    int32_t movingOff = -1, dirOff = -1, alphaOff = -1, attachOff = -1;
    uint8_t movingMask = 0, dirMask = 0;
    bool    ok() const { return movingOff >= 0 && dirOff >= 0 && alphaOff >= 0; }
};
Fields g_f;

const Fields& FieldsOf(void* lift) {
    void* const cls = R::ClassOf(lift);
    if (cls == g_f.cls) return g_f;
    g_f = Fields{};
    g_f.cls = cls;
    if (!R::FindBoolProperty(cls, N::CargoLiftMovingProp, g_f.movingOff, g_f.movingMask)) g_f.movingOff = -1;
    if (!R::FindBoolProperty(cls, N::CargoLiftDirProp, g_f.dirOff, g_f.dirMask)) g_f.dirOff = -1;
    g_f.alphaOff = R::FindPropertyOffset(cls, N::CargoLiftAlphaProp);
    g_f.attachOff = R::FindPropertyOffset(cls, N::CargoLiftAttachProp);
    if (!g_f.ok())
        UE_LOGE("cargo_lift: %ls's fields did not resolve (moving %d dir %d alpha %d)", R::ClassNameOf(lift).c_str(),
                g_f.movingOff, g_f.dirOff, g_f.alphaOff);
    return g_f;
}

bool ReadBit(void* obj, int32_t off, uint8_t mask) {
    return (*(reinterpret_cast<const uint8_t*>(obj) + off) & mask) != 0;
}

void WriteBit(void* obj, int32_t off, uint8_t mask, bool v) {
    uint8_t* b = reinterpret_cast<uint8_t*>(obj) + off;
    *b = v ? static_cast<uint8_t>(*b | mask) : static_cast<uint8_t>(*b & ~mask);
}

}  // namespace

bool IsLift(void* obj) {
    if (!obj) return false;
    void* const lc = LiftClass();
    if (!lc) return false;
    void* const cls = R::ClassOf(obj);
    return cls && R::IsDescendantOfAny(cls, &lc, 1);
}

int32_t Find(void** out, int32_t cap) {
    int32_t n = 0;
    if (!out || cap <= 0) return 0;
    n = world_instances::Find(N::CargoLiftClass, out, cap);
    if (n < cap) {
        // A placed prop of the lift may be a class under it: those are instances of their own class.
        void* more[8] = {};
        const int32_t m = world_instances::Find(N::CargoLiftPropClass, more, 8);
        for (int32_t i = 0; i < m && n < cap; ++i)
            if (IsLift(more[i])) out[n++] = more[i];
    }
    return n;
}

bool ReadState(void* lift, State& out) {
    if (!IsLift(lift) || !R::IsLive(lift)) return false;
    const Fields& f = FieldsOf(lift);
    if (!f.ok()) return false;
    out.moving = ReadBit(lift, f.movingOff, f.movingMask);
    out.dir = ReadBit(lift, f.dirOff, f.dirMask);
    std::memcpy(&out.alpha, reinterpret_cast<const uint8_t*>(lift) + f.alphaOff, sizeof(float));
    return true;
}

int ReadPressAction(const script_gate::Call& call) {
    if (!call.function || !call.locals) return -1;
    static void*   sFn = nullptr;
    static int32_t sOff = -1;
    if (call.function != sFn) {
        sFn = call.function;
        sOff = R::FindParamOffset(call.function, N::CargoLiftPressActionParam);
    }
    return sOff >= 0 ? static_cast<int>(call.locals[sOff]) : -1;
}

bool Press(void* lift, void* player, uint8_t action) {
    if (!IsLift(lift)) return false;
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(lift), N::CargoLiftPressFn);
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    if (!f.Set<void*>(N::CargoLiftPressPlayerParam, player) ||
        !f.Set<uint8_t>(N::CargoLiftPressActionParam, action))
        return false;
    return Call(lift, f);
}

bool SnapToEnd(void* lift, bool dir, float alpha) {
    if (!IsLift(lift) || !R::IsLive(lift)) return false;
    const Fields& f = FieldsOf(lift);
    if (!f.ok() || f.attachOff < 0) return false;
    const float end = alpha >= 0.5f ? 1.f : 0.f;
    void* attach = nullptr;
    std::memcpy(&attach, reinterpret_cast<const uint8_t*>(lift) + f.attachOff, sizeof(attach));
    if (!attach || !R::IsLive(attach)) return false;
    WriteBit(lift, f.dirOff, f.dirMask, dir);
    WriteBit(lift, f.movingOff, f.movingMask, false);
    std::memcpy(reinterpret_cast<uint8_t*>(lift) + f.alphaOff, &end, sizeof(float));
    return ue_wrap::engine::SetComponentRelativeLocation(attach, FVector{0.f, 0.f, end * N::CargoLiftTravelUU});
}

}  // namespace ue_wrap::cargo_lift
