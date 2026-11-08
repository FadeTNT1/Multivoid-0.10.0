// ue_wrap/actors/eyer.h -- engine access for the night stalker (eyer_C): reflected reads of the members the coop
// layer judges it by, and of the arguments of its kill sphere's overlap. Engine-wrapper layer (principle 7): no
// gameplay, network or coop state. Game thread. Implementation: src/ue_wrap/actors/eyer.cpp.

#pragma once

#include "ue_wrap/core/script_gate.h"

namespace ue_wrap::eyer {

// The eyer's `isAngry` (true from goAngry until a failed stare ends it); false for null, a dead actor, or a class
// that has no such member. Game thread.
bool IsAngry(void* eyer);

// The OtherActor of a kill-sphere begin-overlap call (the actor that touched the sphere), or null when the
// parameter does not resolve. For a script-gate watch on the class's bound-event body. Game thread.
void* OverlapOtherActor(const script_gate::Call& call);

}  // namespace ue_wrap::eyer
