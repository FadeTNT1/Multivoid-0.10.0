// ue_wrap/world/water_float_master.cpp -- see ue_wrap/world/water_float_master.h.

#include "ue_wrap/world/water_float_master.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>
#include <cstring>

namespace ue_wrap::water_float_master {
namespace {

namespace R = reflection;
namespace P = profile;

// A TArray<AActor*> as the engine lays it out.
struct ActorArray {
    void**  data;
    int32_t num;
    int32_t max;
};

constexpr int32_t kMaxActors = 8192;   // far past any real count: a garbage read is not walked

void*   g_cls = nullptr;     // the class the offset was resolved on
int32_t g_off = -1;
bool    g_said = false;

}  // namespace

size_t ActorsInWater(std::vector<void*>& out) {
    out.clear();
    void* master = world_singleton::Find(P::name::WaterFloatMasterClass);
    if (!master) return 0;
    void* cls = R::ClassOf(master);
    if (cls != g_cls) {
        g_cls = cls;
        g_off = cls ? R::FindPropertyOffset(cls, P::name::WaterFloatMasterActorsProp) : -1;
        if (g_off < 0 && !g_said) {
            g_said = true;
            UE_LOGW("water_float_master: %ls.%ls did not resolve -- the buoyancy feeder is left out",
                    P::name::WaterFloatMasterClass, P::name::WaterFloatMasterActorsProp);
        }
    }
    if (g_off < 0) return 0;
    ActorArray a{};
    std::memcpy(&a, reinterpret_cast<const char*>(master) + g_off, sizeof(a));
    if (!a.data || a.num <= 0 || a.num > kMaxActors || a.max < a.num) return 0;
    out.reserve(static_cast<size_t>(a.num));
    for (int32_t i = 0; i < a.num; ++i)
        if (a.data[i]) out.push_back(a.data[i]);
    return out.size();
}

}  // namespace ue_wrap::water_float_master
