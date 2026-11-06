// ue_wrap/actors/character_push.h -- the force a character's movement component puts on the simulated
// bodies it touches or stands on, read and written by reflection.
//
// A player pawn's UCharacterMovementComponent pushes a simulated prop it walks into (PushForceFactor,
// InitialPushForceFactor, TouchForceFactor) and presses on the one it stands on
// (StandingDownwardForceScale). The values are class defaults of the game's own pawn Blueprint that this mod
// has not read before; the wrapper finds each property by name and type on the live component's class, so no
// offset is guessed, and says once per property when one is missing or is not a float. The remote puppet's
// switch is EnforcePuppetPropGuard (puppet.h); this is the local pawn's dial. No gameplay and no policy.
// Game thread.

#pragma once

namespace ue_wrap::character_push {

// One pawn's push values. A `have*` flag is false when the property did not resolve on this build.
struct PushValues {
    bool  haveInteraction = false;   // bEnablePhysicsInteraction
    bool  interaction = false;
    bool  havePush = false;
    float push = 0.f;                // PushForceFactor
    bool  haveInitialPush = false;
    float initialPush = 0.f;         // InitialPushForceFactor
    bool  haveTouch = false;
    float touch = 0.f;               // TouchForceFactor
    bool  haveStanding = false;
    float standing = 0.f;            // StandingDownwardForceScale
};

// The values `pawn`'s CharacterMovement holds now. False when the pawn or its movement component is not live or
// no property resolved (`out` is then untouched). Game thread.
bool Read(void* pawn, PushValues& out);

// Writes the three push factors and the standing scale that `v` flags as present; bEnablePhysicsInteraction is
// never written here (the puppet guard owns that switch). False when the pawn or its component is not live.
// Game thread.
bool WriteForces(void* pawn, const PushValues& v);

}  // namespace ue_wrap::character_push
