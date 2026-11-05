// ue_wrap/actors/physics_impact.h -- a prop's comp_physicsImpact_C as a caller drives it: the component, and its own
// `impactDamage(damage, impact, hit, actor)`, the entry a physics impact's damage ends in (health, the damaged and
// destroyed cues, the break). Engine-wrapper layer (principle 7): one reflected read and one UFunction call, with no
// gameplay, network or coop state. Game thread. Implementation: src/ue_wrap/actors/physics_impact.cpp.

#pragma once

#include "ue_wrap/core/types.h"

namespace ue_wrap::physics_impact {

// `prop`'s physicsImpact component (prop_C.physicsImpact), live, or null for an actor with none.
void* ComponentOf(void* prop);

// `prop`'s component's own impactDamage, as an impact would call it: `damage` and `impact` as the parameters, the hit
// built on the prop's StaticMesh at `hitLoc` (a blocking hit), `causer` the hitting actor (null for none). False when
// the prop has no component, the component no such function, the hit cannot be built, or the call faulted.
bool InvokeImpactDamage(void* prop, float damage, const FVector& impact, const FVector& hitLoc, void* causer);

}  // namespace ue_wrap::physics_impact
