// ue_wrap/actors/physics_impact.cpp -- see ue_wrap/actors/physics_impact.h.

#include "ue_wrap/actors/physics_impact.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/hit_result.h"

#include <cstdint>

namespace ue_wrap::physics_impact {
namespace {

namespace R = reflection;
namespace N = profile::name;

}  // namespace

void* ComponentOf(void* prop) {
    if (!prop || !R::IsLive(prop)) return nullptr;
    static R::InstanceOffset sComp(N::PropPhysicsImpactProp);
    const int32_t off = sComp.Of(prop);
    if (off < 0) return nullptr;
    void* comp = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(prop) + off);
    return (comp && R::IsLive(comp)) ? comp : nullptr;
}

bool InvokeImpactDamage(void* prop, float damage, const FVector& impact, const FVector& hitLoc, void* causer) {
    void* comp = ComponentOf(prop);
    if (!comp) return false;
    void* cls = R::ClassOf(comp);
    void* fn = cls ? R::FindDispatchFunctionCached(cls, N::PhysImpactDamageFn) : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    void* mesh = ue_wrap::prop::GetStaticMesh(prop);
    if (mesh && !R::IsLive(mesh)) mesh = nullptr;
    if (!hit_result::Write(f, L"hit", prop, mesh, hitLoc)) return false;
    f.Set<float>(L"damage", damage);
    f.Set<FVector>(L"impact", impact);
    f.Set<void*>(L"actor", causer && R::IsLive(causer) ? causer : nullptr);
    return Call(comp, f);
}

}  // namespace ue_wrap::physics_impact
