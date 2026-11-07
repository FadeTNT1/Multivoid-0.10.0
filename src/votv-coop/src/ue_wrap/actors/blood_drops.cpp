// ue_wrap/actors/blood_drops.cpp -- see ue_wrap/actors/blood_drops.h.

#include "ue_wrap/actors/blood_drops.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstring>

namespace ue_wrap::blood_drops {
namespace {

namespace R = ue_wrap::reflection;
namespace N = ue_wrap::profile::name;

// The gamemode's master field, resolved per gamemode class.
void*   g_gmCls = nullptr;
int32_t g_masterOff = -1;

// addParticle, resolved per master class; its frame offsets per function.
void* g_masterCls = nullptr;
void* g_addFn = nullptr;

struct Offsets {
    void*   fn = nullptr;
    int32_t location = -1, velocity = -1, size = -1, type = -1;
};
Offsets g_read;

void* Master() {
    void* const gm = world_singleton::Gamemode();
    if (!gm) return nullptr;
    if (void* const cls = R::ClassOf(gm); cls != g_gmCls) {
        g_gmCls = cls;
        g_masterOff = R::FindPropertyOffset(cls, N::GamemodeBloodDecalsProp);
        if (g_masterOff < 0)
            UE_LOGE("blood_drops: %ls.%ls did not resolve", R::ClassNameOf(gm).c_str(), N::GamemodeBloodDecalsProp);
    }
    if (g_masterOff < 0) return nullptr;
    void* master = nullptr;
    std::memcpy(&master, static_cast<const uint8_t*>(gm) + g_masterOff, sizeof(master));
    return master && R::IsLive(master) ? master : nullptr;
}

}  // namespace

bool ReadAddParticle(const script_gate::Call& c, Drop* out) {
    if (!c.function || !c.locals || !out) return false;
    if (c.function != g_read.fn) {
        g_read = Offsets{};
        g_read.fn = c.function;
        g_read.location = R::FindParamOffset(c.function, N::BloodAddParticleLocationParam);
        g_read.velocity = R::FindParamOffset(c.function, N::BloodAddParticleVelocityParam);
        g_read.size = R::FindParamOffset(c.function, N::BloodAddParticleSizeParam);
        g_read.type = R::FindParamOffset(c.function, N::BloodAddParticleTypeParam);
        if (g_read.location < 0 || g_read.velocity < 0 || g_read.size < 0 || g_read.type < 0)
            UE_LOGE("blood_drops: %ls's parameters did not resolve (location %d velocity %d size %d type %d)",
                    N::BloodAddParticleFn, g_read.location, g_read.velocity, g_read.size, g_read.type);
    }
    if (g_read.location < 0 || g_read.velocity < 0 || g_read.size < 0 || g_read.type < 0) return false;
    std::memcpy(&out->location, c.locals + g_read.location, sizeof(FVector));
    std::memcpy(&out->velocity, c.locals + g_read.velocity, sizeof(FVector));
    std::memcpy(&out->size, c.locals + g_read.size, sizeof(float));
    std::memcpy(&out->type, c.locals + g_read.type, sizeof(int32_t));
    return true;
}

bool AddParticle(const Drop& d) {
    void* const master = Master();
    if (!master) return false;
    if (void* const cls = R::ClassOf(master); cls != g_masterCls) {
        g_masterCls = cls;
        g_addFn = R::FindDispatchFunctionCached(cls, N::BloodAddParticleFn);
        if (!g_addFn)
            UE_LOGE("blood_drops: %ls.%ls did not resolve", R::ClassNameOf(master).c_str(), N::BloodAddParticleFn);
    }
    if (!g_addFn) return false;
    ParamFrame f(g_addFn);
    if (!f.valid()) return false;
    if (!f.Set<FVector>(N::BloodAddParticleLocationParam, d.location) ||
        !f.Set<FVector>(N::BloodAddParticleVelocityParam, d.velocity) ||
        !f.Set<float>(N::BloodAddParticleSizeParam, d.size) || !f.Set<int32_t>(N::BloodAddParticleTypeParam, d.type))
        return false;
    return Call(master, f);
}

}  // namespace ue_wrap::blood_drops
