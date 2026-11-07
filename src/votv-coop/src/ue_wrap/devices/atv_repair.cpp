// ue_wrap/devices/atv_repair.cpp -- see ue_wrap/devices/atv_repair.h.

#include "ue_wrap/devices/atv_repair.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"

namespace ue_wrap::atv_repair {
namespace {

namespace R = ue_wrap::reflection;
namespace N = ue_wrap::profile::name;

bool g_tried = false;
bool g_ok = false;
int32_t g_offTire = -1, g_offSocket = -1, g_offWheel = -1;
int32_t g_offSpareBox = -1, g_offContainer = -1;
uint8_t g_maskSpareBox = 0, g_maskContainer = 0;
void* g_fnUnscrew = nullptr;
void* g_fnSpawn = nullptr;
void* g_fnReset = nullptr;

bool ReadBit(const void* atv, int32_t off, uint8_t mask) {
    return (*(reinterpret_cast<const uint8_t*>(atv) + off) & mask) != 0;
}

}  // namespace

bool Resolve() {
    if (g_ok) return true;
    if (g_tried) return false;
    void* cls = ue_wrap::object_index::ClassByName(L"ATV_C");
    if (!cls) return false;
    g_tried = true;
    g_offTire   = R::FindPropertyOffset(cls, N::AtvLookAtTireProp);
    g_offSocket = R::FindPropertyOffset(cls, N::AtvLookAtTireSocketProp);
    g_offWheel  = R::FindPropertyOffset(cls, N::AtvPlayersWheelProp);
    const bool boxOk = R::FindBoolProperty(cls, N::AtvLookAtSpareBoxProp, g_offSpareBox, g_maskSpareBox) &&
                       g_offSpareBox >= 0 && g_maskSpareBox != 0;
    const bool contOk = R::FindBoolProperty(cls, N::AtvLookAtContainerProp, g_offContainer, g_maskContainer) &&
                        g_offContainer >= 0 && g_maskContainer != 0;
    g_fnUnscrew = R::FindFunction(cls, N::AtvUnscrewPanelFn);
    g_fnSpawn   = R::FindFunction(cls, N::AtvGetUnscrewSpawnFn);
    g_fnReset   = R::FindFunction(cls, N::AtvResetUnscrewFn);
    g_ok = g_offTire >= 0 && g_offSocket >= 0 && g_offWheel >= 0 && boxOk && contOk && g_fnUnscrew && g_fnSpawn &&
           g_fnReset;
    if (!g_ok)
        UE_LOGE("atv_repair: resolve INCOMPLETE (lookAtTire %d, lookAtTireSocket %d, playersWheel %d, spare box %d, "
                "container %d, unscrewPanel %d, getUnscrewSpawn %d, resetUnscrew %d) -- an ATV unscrew runs where it "
                "is made, as before",
                g_offTire, g_offSocket, g_offWheel, boxOk ? 1 : 0, contOk ? 1 : 0, g_fnUnscrew ? 1 : 0,
                g_fnSpawn ? 1 : 0, g_fnReset ? 1 : 0);
    return g_ok;
}

bool ReadLook(void* atv, Look& out) {
    out = Look{};
    if (!atv || !Resolve()) return false;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(atv);
    out.tire = *reinterpret_cast<const int32_t*>(base + g_offTire);
    out.socket = *reinterpret_cast<const int32_t*>(base + g_offSocket);
    out.atSpareBox = ReadBit(atv, g_offSpareBox, g_maskSpareBox);
    out.atContainer = ReadBit(atv, g_offContainer, g_maskContainer);
    return true;
}

bool ReplayUnscrew(void* atv, int32_t tire) {
    if (!atv || !Resolve() || tire < -1 || tire > 3) return false;
    uint8_t* base = reinterpret_cast<uint8_t*>(atv);
    *reinterpret_cast<int32_t*>(base + g_offTire) = tire;
    *reinterpret_cast<int32_t*>(base + g_offSocket) = -1;
    *reinterpret_cast<void**>(base + g_offWheel) = nullptr;
    ParamFrame spawn(g_fnSpawn);
    if (!spawn.valid() || !Call(atv, spawn)) return false;
    void* spawnAt = spawn.Get<void*>(L"ReturnValue");
    if (!spawnAt) return false;
    ParamFrame unscrew(g_fnUnscrew);
    if (!unscrew.valid() || !unscrew.Set<void*>(N::AtvUnscrewSpawnAtParam, spawnAt)) return false;
    return Call(atv, unscrew);
}

bool ResetUnscrew(void* atv) {
    if (!atv || !Resolve()) return false;
    ParamFrame f(g_fnReset);
    return f.valid() && Call(atv, f);
}

}  // namespace ue_wrap::atv_repair
