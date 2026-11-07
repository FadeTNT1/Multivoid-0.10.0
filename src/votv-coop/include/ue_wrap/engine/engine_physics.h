// ue_wrap/engine/engine_physics.h -- a primitive component's rigid body: simulation on or off,
// whether it simulates, its linear and angular velocity, and its centre of mass. Engine-wrapper layer (principle 7):
// each call is one reflected UFunction call on the component, with a stack parameter frame and no
// allocation, so the held-prop drive can make one every tick. Each UFunction resolves once; one
// that does not resolve is said once and its call is a no-op from then on. Game thread.
// Implementation: src/ue_wrap/engine/engine_physics.cpp.

#pragma once

#include "ue_wrap/core/types.h"

namespace ue_wrap::engine {

// A live UPrimitiveComponent, the only thing the calls below dispatch on: a UPrimitiveComponent function run on
// anything else runs on the wrong layout.
bool IsPrimitiveComponent(void* component);

// UPrimitiveComponent::SetSimulatePhysics. A no-op on anything but a live primitive component, as each call here is.
// True when the call ran: a live primitive component, the UFunction resolved, the dispatch made.
bool SetComponentSimulatePhysics(void* component, bool simulate);

// SetPhysicsLinearVelocity (cm/s) and SetPhysicsAngularVelocityInDegrees, replacing the body's own
// (bAddToCurrent false). A kinematic body ignores both: switch its simulation on first. True when the call ran.
bool SetComponentLinearVelocity(void* component, float vx, float vy, float vz);
bool SetComponentAngularVelocity(void* component, float wx, float wy, float wz);

// IsSimulatingPhysics: USceneComponent's UFunction, which UPrimitiveComponent overrides natively.
// False for anything but a live primitive component, or when it did not resolve.
bool IsComponentSimulatingPhysics(void* component);

// GetCenterOfMass: the body's centre of mass in world space; for a body others are welded into, the whole
// weld's. False for anything but a live primitive component or when it did not resolve, `out` left as it was.
bool GetComponentCenterOfMass(void* component, FVector& out);

// SetEnableGravity and IsGravityEnabled: whether the body falls. True when the call ran (the read: `enabled` written),
// false for anything but a live primitive component or when the UFunction did not resolve.
bool SetComponentEnableGravity(void* component, bool enabled);
bool ReadComponentGravityEnabled(void* component, bool& enabled);

// WakeRigidBody (the whole body): a body asleep in PhysX stays where it is when its gravity comes back on, so a
// prop that came to rest while floating hangs in the air. True when the call ran.
bool WakeComponentBody(void* component);

// Gives a floated body back to the world: gravity on when it was off, and the body woken when it simulates, so it
// falls. A component that is not a live primitive does nothing. `gravityRestored` is true when the gravity was off
// and now is on, `woken` when the body simulates and was woken.
struct GravityRelease {
    bool gravityRestored = false;
    bool woken = false;
};
GravityRelease ReleaseComponentGravity(void* component);

// Every primitive component the actor's Blueprint chain declares as a member (a component variable) that
// simulates is stopped; returns how many were. For an actor driven from elsewhere, which must not also
// simulate. The members are found once per class name, a Blueprint's layout being the same in every world.
// Null-safe.
int StopActorSimulating(void* actor);

}  // namespace ue_wrap::engine
