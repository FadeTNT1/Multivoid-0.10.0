// ue_wrap/world/profile.h -- the player's own profile save, mainGamemode.save_main (a save_main_C): the
// days its player has lived through and its achievements, which every machine keeps for its own player
// and the day's rollover writes. Engine-wrapper layer (principle 7): no network or gameplay logic.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ue_wrap::profile {

// save_main.stats' days_total on the running world's gamemode. False while there is no gamemode or
// profile, or when a member does not resolve (said once). Game thread.
bool ReadDaysTotal(int32_t& out);

// days_total += n, as the rollover adds one a midnight. Game thread.
bool AddDaysTotal(int32_t n);

// save_main.stats' signals_found, the signals this machine's player has found, which a catch in the renderer's
// gatherSignal adds to on the machine that rolled it (spaceRenderer.cpp:843-849). False as ReadDaysTotal is. Game
// thread.
bool ReadSignalsFound(int32_t& out);

// signals_found += n. Game thread.
bool AddSignalsFound(int32_t n);

// save_main.stats' signals_processed, the signals this machine's player has refined to the last level, which the
// refiner's completion adds to on the machine that decoded it (analogDScreenTest ubergraph @71094). False as
// ReadDaysTotal is. Game thread.
bool ReadSignalsProcessed(int32_t& out);

// The stat named as its stats member begins (days_total, signals_found, signals_processed) += n. False for any
// other name, or as ReadDaysTotal is. Game thread.
bool AddStat(std::wstring_view name, int32_t n);

// The key this machine's player bound to the input setting `name` (as "coord_ping"), as the game displays it:
// save_main.keybinds_keys at the index `name` has in keybindsNames, the pair the game fills together
// (mainGamemode.cpp:1757-1765). False when `name` is not bound or a member does not resolve. Game thread.
bool KeybindDisplayName(const wchar_t* name, std::wstring& out);

// save_main_C::progressAchievement(name, popup, autosave = false), which lib_C's own entry forwards to:
// the achievement progresses on this machine's profile, with its popup unless `popup` is false. False when it
// does not resolve. Game thread.
bool ProgressAchievement(const wchar_t* name, bool popup = true);

// The running world's profile object (a save_main_C), or null while there is no gamemode or profile. Game thread.
void* Save();

// save_main_C::save(): the profile written to its slot. The profile is the player's own file, not a world save,
// so it is the one save a client of a session may make. False when it does not resolve. Game thread.
bool SaveProfile();

}  // namespace ue_wrap::profile
