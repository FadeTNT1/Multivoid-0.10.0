// ue_wrap/actors/wend_skull.cpp -- see ue_wrap/actors/wend_skull.h.

#include "ue_wrap/actors/wend_skull.h"

#include "ue_wrap/actors/prop_fire.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

namespace ue_wrap::wend_skull {

bool IsWendSkull(void* obj) {
    if (!obj || !reflection::IsLive(obj)) return false;
    void* base = object_index::ClassByName(profile::name::WendSkullClass);
    void* cls = reflection::ClassOf(obj);
    return base && cls && reflection::IsDescendantOfAny(cls, &base, 1);
}

bool Ignite(void* obj) {
    return IsWendSkull(obj) && prop::CallIgnite(obj, profile::name::WendSkullMicrowaveFuel);
}

}  // namespace ue_wrap::wend_skull
