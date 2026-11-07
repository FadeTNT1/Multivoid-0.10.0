// ue_wrap/actors/blood_drops.h -- the gamemode's blood drop maker (engine substrate): read a drop the game hands
// to bloodDecalMaster_C.addParticle, and hand it one of ours.
//
// The gamemode holds one bloodDecalMaster_C (`masteBloodDecals`). Its addParticle(location, velocity, size, type)
// adds a falling sphere with a drip emitter; the master's tick traces each one and, where it lands, spawns a
// grime_blood2 decal (type 0 the drip, 1 the splat), a splat emitter and the drop cue. Every source of floor blood
// in 0.10.0 goes through it: a player's bleeding (effect_bloodLoss_C.dropTimer), a zombie's damage, a gore
// slither, a blood pipe, a gib. Game thread only. No network, coop or policy logic (principle 7).

#pragma once

#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"

#include <cstdint>

namespace ue_wrap::blood_drops {

struct Drop {
    FVector location{};
    FVector velocity{};
    float   size = 0.f;
    int32_t type = 0;
};

// The arguments of a script-gate call on bloodDecalMaster_C.addParticle, read from its frame. False when a
// parameter does not resolve on the called function.
bool ReadAddParticle(const script_gate::Call& call, Drop* out);

// gamemode.masteBloodDecals.addParticle(location, velocity, size, type). False, calling nothing, when the
// gamemode, its master or the function does not resolve, or a parameter does not set.
bool AddParticle(const Drop& drop);

}  // namespace ue_wrap::blood_drops
