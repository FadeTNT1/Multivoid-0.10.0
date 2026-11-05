// ui/host_create_world.h -- "Create world" in the hosting window: the game's own create pane, with
// its own name field and its own world-rules editor, borrowed for one creation and handed back.
//
// The game's save menu (ui_saveSlots_C) is a child of the same menu switcher the hosting window
// lives in. Begin shows it and runs the handler of its "create" button, which opens its create
// pane and loads the rules editor with the defaults, exactly as a click there does; the player
// names the world, sets the rules and presses the game's own Create, whose handler makes the world
// folder through lib_C::createSaveSlotTreeDirectory and stamps the rules, the starter map and the
// start day into the new metadata. The pane then returns to the save menu's list, which is the
// edge Poll watches: a world folder that was not there at Begin is the created world; none, or the
// player leaving the save menu, is a cancel. Either way the hosting window takes the switcher back.
// Main-menu ticks only (game thread).

#pragma once

#include <string>

namespace ui::host_create_world {

// Show the game's save menu with its create pane open. False, nothing changed, when the save menu
// or its handler cannot be found in this menu.
bool Begin(void* menuSwitcher);

enum class State { Idle, Running, Created, Cancelled };

// One menu tick while a creation runs. Created and Cancelled are reported once, then Idle; on
// Created `folder` is the new world's folder. The caller restores its own switcher index.
State Poll(void* menuSwitcher, std::wstring& folder);

bool Running();

// Forget a creation in flight: the menu it ran in is gone.
void Reset();

}  // namespace ui::host_create_world
