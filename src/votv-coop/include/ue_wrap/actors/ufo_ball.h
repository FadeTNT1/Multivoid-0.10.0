// ue_wrap/actors/ufo_ball.h -- engine access for the gamemode's ball UFO (ufo_ballfo_C): reflected reads and writes of
// the components its `set` randomizes. Engine-wrapper layer (principle 7): no gameplay, network or coop state. Game
// thread. Implementation: src/ue_wrap/actors/ufo_ball.cpp.
//
// The actor's `set` (run by its construction script and again by its BeginPlay) turns the `center` component to a random
// yaw and places the endpoints `a` and `b` relative to it; the class's tick then slides the `balls` component toward `b`
// at a constant speed and destroys the actor on arrival. Those few values are everything that is not deterministic.

#pragma once

#include "ue_wrap/core/types.h"

namespace ue_wrap::ufo_ball {

// What `set` randomized, plus the ball's place now.
struct Setup {
    float    centerYaw = 0.f;   // center's relative yaw, degrees
    float    aX = 0.f, aY = 0.f;  // endpoint a's relative location (z is always 0)
    float    bX = 0.f, bY = 0.f;  // endpoint b's relative location (z is always 0)
    FVector  balls{};           // the ball component's world location
};

// True if `actor`'s class is exactly ufo_ballfo_C. Game thread.
bool IsUfoBall(void* actor);

// Read the actor's setup. False (out untouched) when the actor is no live ufo_ballfo_C or a component does not
// resolve; the reason is logged once per missing member. Game thread.
bool Read(void* actor, Setup& out);

// Write a setup onto a ufo_ballfo_C, through the engine's setters. False when the actor is no live ufo_ballfo_C
// or a component does not resolve. Game thread.
bool Apply(void* actor, const Setup& in);

}  // namespace ue_wrap::ufo_ball
