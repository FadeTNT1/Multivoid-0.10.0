// coop/player/local_push.h -- how hard the LOCAL player's own body pushes simulated props, for the length of a session.
//
// The remote puppet is already kept off props (ue_wrap::puppet::EnforcePuppetPropGuard). The local pawn is not: its
// CharacterMovementComponent shoves the simulated prop it walks into (PushForceFactor, InitialPushForceFactor,
// TouchForceFactor) and presses on the one it stands on (StandingDownwardForceScale), and in a session that is the
// "finicky" prop a tester reports: it jitters under a crouch, slides when stood on, or is flung at an edge. Every pawn
// generation this module reads the values (once, by reflection, logged), then holds them at the configured fraction
// of what it read -- `local_push_scale` for the three push factors, `local_stand_force_scale` for the standing force --
// and writes the game's own values back when the session ends. bEnablePhysicsInteraction stays on: the player can
// still push a prop on purpose. Both roles; a session only, so single-player is never touched.
//
// Game thread.

#pragma once

namespace coop::local_push {

// The fraction of `base` that `scale` leaves, clamped to [0, 1] of it. Pure, so it is judged at compile time below.
constexpr float Scaled(float base, float scale) {
    return base * (scale < 0.f ? 0.f : scale > 1.f ? 1.f : scale);
}
static_assert(Scaled(750000.f, 0.25f) == 187500.f, "a quarter of the push factor");
static_assert(Scaled(1.f, 0.f) == 0.f, "scale 0 removes the force");
static_assert(Scaled(2.f, 5.f) == 2.f, "a scale above 1 never raises the game's own value");
static_assert(Scaled(2.f, -1.f) == 0.f, "a negative scale is zero");

// Per gameplay tick while a session runs. A new pawn generation is read at once; afterwards the pawn is checked
// about once a second, so a config edit lands without a restart. Game thread.
void Tick();

// Session end: the game's own values go back onto the live pawn and the state is dropped. Game thread.
void OnDisconnect();

}  // namespace coop::local_push
