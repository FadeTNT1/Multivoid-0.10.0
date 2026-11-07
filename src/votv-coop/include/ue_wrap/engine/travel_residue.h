// ue_wrap/engine/travel_residue.h -- the one-way writes a level-travel trigger makes before its
// lib_C::loadLevel, read and put back when that travel is cancelled.
//
// A trigger that sends the player to another level prepares the trip first: it clears the game
// instance's loadedSlotName (skullzone_C, insomniac_C), names no current level (prop_biotoilet_C,
// puddle_C) or stops the player's movement (puddle_C). The travel would have replaced all of it;
// a cancelled travel leaves it standing, and an empty loadedSlotName makes saveSlot_C::save return
// before it writes anything. The names are in sdk_profile_names.h.
//
// Engine-wrapper layer (principle 7): no gameplay or co-op state. Game thread only.

#pragma once

#include <string>

namespace ue_wrap::travel_residue {

// The game instance's loadedSlotName: the save entry the game writes to. Empty when it is empty or
// does not resolve (`ok` false then).
std::wstring ReadLoadedSlotName(bool& ok);

// Put `name` back as the game instance's loadedSlotName. False when it does not resolve or the
// string could not be made.
bool WriteLoadedSlotName(const std::wstring& name);

// When the game instance's currentLevel reads None, name `level` there. `wasNone` says whether it
// did. False when the property does not resolve or the name cannot be made yet.
bool RestoreCurrentLevelIfNone(const wchar_t* level, bool& wasNone);

// When `character`'s movement mode is MOVE_None, set it to MOVE_Walking through the engine's own
// SetMovementMode. `wasStopped` says whether it was. False when the movement component or the verb
// does not resolve.
bool RestoreWalkingIfStopped(void* character, bool& wasStopped);

}  // namespace ue_wrap::travel_residue
