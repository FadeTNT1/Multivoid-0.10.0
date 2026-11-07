// ue_wrap/actors/cargo_lift.h -- the cargo lift (cargolift_C), engine substrate: find the world's lifts, read the
// state its press moves, press it, and put its car at an end.
//
// A lift's car is its `attach` component, moved along Z (0..500 cm) by a latent loop that actionOptionIndex starts
// (action 4): `dir` flips, `moving` is set, `alpha` runs 0..1 (dir true) or 1..0 and `moving` clears at the end. Every
// peer's copy runs that loop on its own; riders ride because the car is a component of the actor they stand on. Game
// thread only. No network, coop or policy logic (principle 7).

#pragma once

#include "ue_wrap/core/script_gate.h"

#include <cstdint>

namespace ue_wrap::cargo_lift {

struct State {
    bool  moving = false;
    bool  dir = false;   // true: the trip runs toward alpha 1
    float alpha = 0.f;   // 0 .. 1, the car's place along its travel
};

// Whether `obj` is a cargo lift (cargolift_C or a class under it). False when the class is not loaded.
bool IsLift(void* obj);

// Up to `cap` live lifts of the running world, in no particular order; the count. Zero while the class is not loaded.
int32_t Find(void** out, int32_t cap);

// The lift's moving, dir and alpha, read in place. False when `lift` is not a lift or a field did not resolve.
bool ReadState(void* lift, State& out);

// The interaction action a script-gate call on the lift's actionOptionIndex carries, or -1 when it did not resolve.
int ReadPressAction(const script_gate::Call& call);

// The game's own press: actionOptionIndex(player, <zero hit>, action, <no component>). `player` may be null (the
// body never reads it). False when the verb or a parameter did not resolve, or the call failed.
bool Press(void* lift, void* player, uint8_t action);

// Put the lift at rest at an end: `dir`, `alpha` (clamped to 0 or 1 by the nearer end) and moving false written, and
// the car set to that end's place. False when a field or the car did not resolve.
bool SnapToEnd(void* lift, bool dir, float alpha);

}  // namespace ue_wrap::cargo_lift
