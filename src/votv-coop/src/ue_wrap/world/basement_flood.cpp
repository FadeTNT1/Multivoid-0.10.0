// ue_wrap/world/basement_flood.cpp -- see ue_wrap/world/basement_flood.h.

#include "ue_wrap/world/basement_flood.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>
#include <cstring>

namespace ue_wrap::basement_flood {
namespace {

namespace R = reflection;
namespace P = profile;

// What the class resolved to, kept for the class it was resolved on. A Blueprint class is a new object in a new
// world, so the pointer is compared on every use and a different one resolves again.
struct Resolved {
    void*   cls = nullptr;
    void*   getLevel = nullptr;
    void*   setLevel = nullptr;
    void*   startMoving = nullptr;
    void*   updateData = nullptr;
    int32_t heightOff = -1;
    int32_t distOff = -1;
    int32_t moveToOff = -1;
    uint8_t moveToMask = 0;
    bool    ok = false;
};
Resolved g_r;
bool     g_said = false;

bool EnsureFor(void* flood) {
    void* cls = flood ? R::ClassOf(flood) : nullptr;
    if (!cls) return false;
    if (g_r.cls == cls) return g_r.ok;
    Resolved r;
    r.cls = cls;
    r.getLevel = R::FindFunction(cls, P::name::BasementFloodGetLevelFn);
    r.setLevel = R::FindFunction(cls, P::name::BasementFloodSetLevelFn);
    r.startMoving = R::FindFunction(cls, P::name::BasementFloodStartMovingFn);
    r.updateData = R::FindDispatchFunctionCached(cls, P::name::WaterVolumeUpdateDataFn);
    r.heightOff = R::FindPropertyOffset(cls, P::name::BasementFloodHeightProp);
    r.distOff = R::FindPropertyOffset(cls, P::name::BasementFloodDistProp);
    int32_t off = -1;
    uint8_t mask = 0;
    if (R::FindBoolProperty(cls, P::name::BasementFloodMoveToProp, off, mask) && off >= 0 && mask != 0) {
        r.moveToOff = off;
        r.moveToMask = mask;
    }
    r.ok = r.getLevel && r.setLevel && r.updateData && r.heightOff >= 0 && r.distOff >= 0;
    g_r = r;
    if (!r.ok && !g_said) {
        g_said = true;
        UE_LOGW("basement_flood: %ls did not resolve (getLevel=%p setLevel=%p updateWaterData=%p height@%d dist@%d) -- "
                "the basement flood sync is left out", P::name::BasementFloodClass, r.getLevel, r.setLevel, r.updateData,
                r.heightOff, r.distOff);
    } else if (r.ok) {
        UE_LOGI("basement_flood: resolved %ls getLevel/setLevel/updateWaterData height@0x%04X dist@0x%04X moveTo@%d",
                P::name::BasementFloodClass, static_cast<unsigned>(r.heightOff), static_cast<unsigned>(r.distOff), r.moveToOff);
    }
    return r.ok;
}

}  // namespace

void* Actor() { return world_singleton::Find(P::name::BasementFloodClass); }

bool Read(void* flood, State& out) {
    if (!EnsureFor(flood)) return false;
    float dist = 0.f;
    std::memcpy(&dist, reinterpret_cast<const char*>(flood) + g_r.distOff, sizeof(float));
    if (!(dist > 0.f)) return false;   // not begun play: loc_a is still the origin
    ParamFrame f(g_r.getLevel);
    if (!Call(flood, f)) return false;
    State s;
    s.begun = true;
    s.level = f.Get<float>(P::name::BasementFloodLevelParam);
    std::memcpy(&s.height, reinterpret_cast<const char*>(flood) + g_r.heightOff, sizeof(float));
    if (g_r.moveToOff >= 0)
        s.moveTo = (static_cast<uint8_t>(reinterpret_cast<const char*>(flood)[g_r.moveToOff]) & g_r.moveToMask) != 0;
    out = s;
    return true;
}

bool ApplyLevel(void* flood, float level) {
    if (!EnsureFor(flood)) return false;
    ParamFrame set(g_r.setLevel);
    set.Set<float>(P::name::BasementFloodAlphaParam, level);
    if (!Call(flood, set)) return false;
    ParamFrame upd(g_r.updateData);
    return Call(flood, upd);
}

namespace {
int32_t DrainBlockageOffset(void* drain) {
    static void*   sCls = nullptr;
    static int32_t sOff = -1;
    void* cls = R::ClassOf(drain);
    if (cls != sCls) {
        sCls = cls;
        sOff = cls ? R::FindPropertyOffset(cls, P::name::BasementDrainBlockageProp) : -1;
    }
    return sOff;
}
}  // namespace

bool ReadBlockage(float& out) {
    void* drain = world_singleton::Find(P::name::BasementDrainClass);
    const int32_t off = drain ? DrainBlockageOffset(drain) : -1;
    if (off < 0) return false;
    std::memcpy(&out, reinterpret_cast<const char*>(drain) + off, sizeof(float));
    return true;
}

bool WriteBlockage(float value) {
    void* drain = world_singleton::Find(P::name::BasementDrainClass);
    const int32_t off = drain ? DrainBlockageOffset(drain) : -1;
    if (off < 0) return false;
    std::memcpy(reinterpret_cast<char*>(drain) + off, &value, sizeof(float));
    return true;
}

bool ApplyMoveTo(void* flood, bool moveTo) {
    if (!EnsureFor(flood) || !g_r.startMoving) return false;
    ParamFrame f(g_r.startMoving);
    f.Set<bool>(P::name::BasementFloodMoveToProp, moveTo);
    return Call(flood, f);
}

}  // namespace ue_wrap::basement_flood
