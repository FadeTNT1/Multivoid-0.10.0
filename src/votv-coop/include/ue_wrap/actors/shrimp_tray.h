// ue_wrap/actors/shrimp_tray.h -- engine access for the shrimp tray (prop_food_shrimp_C), a food whose `shrimps`
// portions are drawn as instances by its `upd()`. Principle-7 engine-wrapper layer: no network or coop state.
//
// Its action 4 and its playerHandUse_RMB take one portion, run upd(), spawn its `shrimpDrop` class 10 cm above the
// tray and, at none left, spawn the prop named shrimp_e in its place and destroy the tray. getData files the count as
// ints[0][0], loadData restores it and runs upd(). The names are in ue_wrap/core/sdk_profile_names.h.

#pragma once

#include <cstdint>

namespace ue_wrap::shrimp_tray {

// True iff `obj` is a live prop_food_shrimp_C or a class below it. Game thread.
bool IsTray(void* obj);

// The tray's `shrimps`. False for anything that is not a tray or a member that did not resolve.
bool ReadCount(void* tray, int32_t& out);

// Write `shrimps` and redraw the portions through the game's own upd(). False, writing nothing, for anything that is
// not a tray; true when the count was written (the redraw is the game's).
bool WriteCountAndRedraw(void* tray, int32_t count);

// The tray's `shrimpDrop` class, as the tray holds it, or null. Read off the live tray, never assumed.
void* DropClass(void* tray);

// Run the tray's own verbs on the host's copy for a client: actionOptionIndex(player, 4) and playerHandUse_RMB(player).
// False when it is not a tray or the call did not run. Game thread.
bool CallDispenseAction(void* tray, void* player, uint8_t action);
bool CallHandUse(void* tray, void* player);

}  // namespace ue_wrap::shrimp_tray
