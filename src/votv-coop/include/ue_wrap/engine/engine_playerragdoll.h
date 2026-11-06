// ue_wrap/engine/engine_playerragdoll.h -- the plushie ragdoll body the game spawns for a faint,
// driven for a puppet. Engine-wrapper layer (principle 7): each call marshals one UFunction call
// or one reflected field access, with no gameplay, network or coop state. Game thread unless a
// declaration says otherwise. Implementation: src/ue_wrap/engine/engine_playerragdoll.cpp.

#pragma once

#include "ue_wrap/core/types.h"

namespace ue_wrap::engine {

// Spawn the game's playerRagdoll_C for `ownerPlayer` as the visible flop body: its Player field is
// stamped before Finish, so BeginPlay configures the mesh from the owner, and then the simulation
// starts. Death-free, since ragdollMode would kill the host. The AActor*, or nullptr. Game thread.
void* SpawnPlayerRagdollBody(void* ownerPlayer, const FVector& location, const FRotator& rotation);

// The native ragdollMode kickoff on a freshly spawned, simulating body: SetPhysicsLinearVelocity on the
// "chest" bone with `chestImpulse` (cm/s) added to the current velocity -- the native recipe uses the
// actor forward vector at -100 cm/s -- and, when `fallVeloc` is non-null, that velocity added to every
// body (fallImpulse's follow-up). False when the mesh or the chest bone is unavailable. Game thread.
bool KickoffRagdollBody(void* body, const FVector& chestImpulse, const FVector* fallVeloc);

// The body's pelvis WORLD rotation (GetSocketRotation "pelvis"); false on a dead body. Used to prove a
// mirror fell over, never to drive it. Game thread.
bool ReadRagdollBodyPelvisRotation(void* body, FRotator& out);

// Attach `actor` to `body`'s mesh at the pelvis bone (KeepWorld), so it follows the flop; Detach
// reverses it. Game thread.
bool AttachActorToRagdollBody(void* actor, void* body);

bool DetachActorFromRagdollBody(void* actor);

// The sender's ragdoll stream: the native ragdoll's pelvis world transform and velocity, in cm/s
// and deg/s. False while not ragdolling. Game thread.
bool ReadLocalRagdollPelvisPhysics(void* mainPlayer, FVector& outLoc, FRotator& outRot,
                                   FVector& outLinVel, FVector& outAngVel);

// The receiver's ragdoll stream: set the mirror body's pelvis linear and angular velocity so it
// tracks the sender's flop. Game thread.
void DriveRagdollBodyPelvisVelocity(void* body, const FVector& linVel, const FVector& angVel);

// True when the body's mesh reports it is simulating physics. When it does not, the sim start
// (collision + SetAllBodiesSimulatePhysics + SetSimulatePhysics) is issued again and `*rearmed`
// is set; the return is the state BEFORE the re-arm. A body left frozen at its spawn transform
// stands upright and rigid, so the spawn and a per-tick watchdog both use this. Game thread.
bool EnsureRagdollBodySimulating(void* body, bool* rearmed);

// A ragdoll actor's simulating SkeletalMesh; null if dead. Game thread.
void* GetRagdollBodyMesh(void* ragdollActor);

// The local player's native ragdoll mesh; the owning field is non-null exactly while ragdolling.
// Game thread.
void* GetLocalRagdollBodyMesh(void* mainPlayer);

}  // namespace ue_wrap::engine
