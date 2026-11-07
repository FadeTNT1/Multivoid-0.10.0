// ue_wrap/actors/fire.h -- the game's fire: a burning patch (fire_C), the flammable component that burns on it and on
// anything else that catches (comp_flammable_C), and the fuel puddle's ignition (grime_gasoline_C). Engine-wrapper
// layer (principle 7): reads, writes and verbs, no gameplay. Names in ue_wrap/core/sdk_profile_names.h (the Fire block).
// Implementation: src/ue_wrap/actors/fire.cpp.

#pragma once

#include "ue_wrap/core/types.h"

namespace ue_wrap::fire {

// The fields a fire_C's begin-play reads, which every producer writes in its deferred window.
struct Params {
    float   fuel = 0.f;
    float   fuelSpeed = 0.f;
    float   fireSize = 0.f;
    float   effectRate = 0.f;
    float   igniteChance = 0.f;
    FVector boxExtent{};
};

// Whether `cls` is fire_C, by its own name. Any thread that may read a class name (game thread in practice).
bool IsFireClass(void* cls);

// Whether `actor` is a fire_C. Game thread.
bool IsFire(void* actor);

// Whether `actor` is a grime_gasoline_C. Game thread.
bool IsGasolineGrime(void* actor);

// Read `fire`'s Params. False when any field does not resolve on its class. Game thread.
bool ReadParams(void* fire, Params& out);

// Write `fire`'s Params, for a fire whose begin-play has not run yet (between the deferred begin and the finish).
// False when any field does not resolve. Game thread.
bool WriteParams(void* fire, const Params& p);

// The comp_flammable_C's `owner` (the actor it burns on, cached at its begin-play), or null. Game thread.
void* FlammableOwner(void* comp);

// The comp_flammable_C's `isBurning`. False when unreadable. Game thread.
bool FlammableBurning(void* comp);

// Write a comp_flammable_C's `fuelSpeed` and `ignitechance`: what its tick burns and what its spread lights. False
// when either does not resolve. Game thread.
bool ParkFlammable(void* comp, float fuelSpeed, float igniteChance);

// The comp_flammable_C's own extinguish(). False when it does not resolve or the call could not run. Game thread.
bool CallExtinguish(void* comp);

// The puddle's `flameBase` (the comp_flammable_C its first ignite added), or null when there is none or it is not
// live. Game thread.
void* GasolineFlameBase(void* grime);

// The puddle's own ignite(fuel). False when it does not resolve or the call could not run. Game thread.
bool CallGasolineIgnite(void* grime, float fuel);

}  // namespace ue_wrap::fire
