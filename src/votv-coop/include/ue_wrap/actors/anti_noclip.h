// ue_wrap/actors/anti_noclip.h -- the reads and verbs under mainPlayer_C's own anti-noclip check, which
// coop::anti_noclip_rescue replaces in a co-op session. Principle-7 engine-wrapper layer: reflection and
// calls, no network, no co-op state.
//
// What the game does. Every tick of the local player traces a line from `lastLoc` (the camera's place when the
// last tick ended) to the camera's place now, against static geometry. When it hits something, the gamemode's
// canBackrooms() is true, the player's capsule ignores collision channel 16 and the view target is the player
// itself, the game decides the player has passed through a wall: with backroomsEnabled (which canBackrooms
// itself requires) it blacks the screen, lifts the player 800 cm, saves, and travels to a backrooms level; the
// other branch teleports the player to the save slot's playerTransform with no check of what stands there. Both
// go through teleportWOBackrooms, which also writes `lastLoc` from the place it set. The names are in
// ue_wrap/core/sdk_profile_names.h.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace ue_wrap::anti_noclip {

// mainPlayer_C.lastLoc. False when the field does not resolve (said once).
bool ReadLastLoc(void* player, FVector& out);

// Overwrite mainPlayer_C.lastLoc (a developer drill's way to stand the check's line through the world).
bool WriteLastLoc(void* player, const FVector& at);

// The response the player's capsule holds for `channel` (ECollisionResponse: 0 ignore, 1 overlap, 2 block).
bool CapsuleResponse(void* player, uint8_t channel, uint8_t& out);

// The capsule's half height, for standing a body on a floor. False when it does not read.
bool CapsuleHalfHeight(void* player, float& out);

// Whether the player controller's view target is the player's own pawn. False when it cannot be read.
bool ViewTargetIsSelf(void* player, bool& out);

// The gamemode's canBackrooms() answered through its own body. `fn` is the UFunction the script gate named.
bool CallCanBackrooms(void* gamemode, void* fn, bool& out);

// Place the player at `loc` (rotation kept) through the game's own teleportWOBackrooms -- the verb that writes
// `lastLoc` and drops a held actor -- and leave it with no velocity and on the ground state, so what carried it
// to the wall does not carry it through the destination. False when the verb did not run.
bool TeleportStill(void* player, const FVector& loc);

}  // namespace ue_wrap::anti_noclip
