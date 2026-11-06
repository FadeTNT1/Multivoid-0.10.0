// ue_wrap/devices/water_shooter.cpp -- see ue_wrap/devices/water_shooter.h.

#include "ue_wrap/devices/water_shooter.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"

#include <chrono>
#include <cstdint>
#include <cstring>

namespace ue_wrap::water_shooter {
namespace {

namespace R  = reflection;
namespace PN = profile::name;

void*   g_washerCls = nullptr;
void*   g_shotCls = nullptr;
void*   g_fieldsCls = nullptr;  // the class the offsets below were resolved on
int32_t g_fireByte = -1;
uint8_t g_fireMask = 0;
int32_t g_offLevel = -1;

FxAssets g_fx{};
bool     g_fxDone = false;
std::chrono::steady_clock::time_point g_nextFxTry{};

bool IsA(void* obj, void*& cls, const wchar_t* name) {
    if (!obj) return false;
    if (!cls) cls = object_index::ClassByName(name);  // one lookup; null while the class is not loaded
    if (!cls) return false;
    void* bases[1] = {cls};
    void* own = R::ClassOf(obj);
    return own && R::IsDescendantOfAny(own, bases, 1);
}

}  // namespace

bool IsWaterShooter(void* actor) { return IsA(actor, g_washerCls, PN::WaterShooterClass); }

bool IsShot(void* obj) { return IsA(obj, g_shotCls, PN::SpongeShotClass); }

bool ReadFiring(void* actor, bool& fire, float& level) {
    if (!actor || !R::IsLive(actor) || !IsWaterShooter(actor)) return false;
    void* cls = R::ClassOf(actor);
    if (cls != g_fieldsCls) {
        g_fieldsCls = cls;
        if (!R::FindBoolProperty(cls, PN::WaterShooterFireProp, g_fireByte, g_fireMask)) g_fireByte = -1;
        g_offLevel = R::FindPropertyOffset(cls, PN::WaterShooterLevelProp);
        UE_LOGI("water_shooter: fields fire@%d/0x%02X level@%d", g_fireByte, g_fireMask, g_offLevel);
    }
    if (g_fireByte < 0 || g_offLevel < 0) return false;
    const uint8_t* base = static_cast<const uint8_t*>(actor);
    fire = (base[g_fireByte] & g_fireMask) != 0;
    std::memcpy(&level, base + g_offLevel, sizeof(level));
    return true;
}

const FxAssets* ResolveFx() {
    if (g_fxDone) {
        // An asset unloaded with its package (a world change) is looked up again.
        if (R::IsLive(g_fx.spray) && R::IsLive(g_fx.splash) && R::IsLive(g_fx.cue)) return &g_fx;
        g_fxDone = false;
        g_fx = FxAssets{};
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextFxTry) return nullptr;
    g_nextFxTry = now + std::chrono::seconds(5);
    if (!g_fx.spray)  g_fx.spray  = R::FindObject(PN::WasherSprayFx, PN::ParticleSystemClass);
    if (!g_fx.splash) g_fx.splash = R::FindObject(PN::WasherSplashFx, PN::ParticleSystemClass);
    if (!g_fx.cue)    g_fx.cue    = R::FindObject(PN::WasherCue, PN::SoundCueClass);
    if (!g_fx.spray || !g_fx.splash || !g_fx.cue) {
        static bool sSaid = false;
        if (!sSaid) {
            sSaid = true;
            UE_LOGI("water_shooter: effect assets not all loaded yet (spray=%p splash=%p cue=%p) -- retrying",
                    g_fx.spray, g_fx.splash, g_fx.cue);
        }
        return nullptr;
    }
    g_fxDone = true;
    UE_LOGI("water_shooter: effect assets spray=%p splash=%p cue=%p", g_fx.spray, g_fx.splash, g_fx.cue);
    return &g_fx;
}

}  // namespace ue_wrap::water_shooter
