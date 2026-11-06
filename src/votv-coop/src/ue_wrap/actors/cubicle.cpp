// ue_wrap/actors/cubicle.cpp -- see ue_wrap/actors/cubicle.h.

#include "ue_wrap/actors/cubicle.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"

#include <cstdint>

namespace ue_wrap::cubicle {
namespace {

namespace R = reflection;
namespace P = profile;

int32_t g_rowBase = 0;  // the ComparisonIndex of "cubicleP"; 0 until resolved

}  // namespace

bool ResolveNames() {
    if (g_rowBase != 0) return true;
    const R::FName n = fname_utils::StringToFName(P::name::CubicleItemRowBase);
    if (n.ComparisonIndex == 0) return false;
    g_rowBase = n.ComparisonIndex;
    return true;
}

bool IsCubicle(void* actor) {
    if (!actor) return false;
    void* cls = R::ClassOf(actor);
    if (void* base = object_index::ClassByName(P::name::CubicleClass); base && prop::WalksToBase(cls, base))
        return true;
    if (g_rowBase == 0) return false;
    // GetPropName answers a zero name for a non-prop, so the row test is the prop test too.
    const R::FName row = prop::GetPropName(actor);
    return row.ComparisonIndex == g_rowBase;
}

}  // namespace ue_wrap::cubicle
