// ue_wrap/actors/wend_skull.h -- engine access for the animal skull (prop_wendskull_C). Engine-wrapper layer
// (principle 7): no network or coop state. What its ignite does is stated at P::name::WendSkullClass.

#pragma once

namespace ue_wrap::wend_skull {

// True iff `obj` is a live prop_wendskull_C or a class below it. Game thread.
bool IsWendSkull(void* obj);

// Run the skull's own ignite(fuel) with the fuel its microwave() passes, on this machine. False when `obj` is not a
// skull or the call did not dispatch. Game thread.
bool Ignite(void* obj);

}  // namespace ue_wrap::wend_skull
