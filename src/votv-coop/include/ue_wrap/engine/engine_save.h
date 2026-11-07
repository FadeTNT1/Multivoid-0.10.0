// ue_wrap/engine/engine_save.h -- the game's own save and travel verbs: loading a slot through its
// entry point, the fresh-game path, and the hook that fires when a save object is ready.
// Engine-wrapper layer (principle 7): each call marshals one UFunction call or one reflected field
// access, with no gameplay, network or coop state. Game thread unless a declaration says
// otherwise. Implementation: src/ue_wrap/engine/engine_save.cpp.

#pragma once

#include <string>
#include <cstdint>
#include <utility>
#include <vector>

namespace ue_wrap::engine {

// AmainGamemode_C::transition("/Game/menu"), the game's own travel verb (the short name does not
// resolve); needs no pause and works for a dead player. Hold game_thread::SetTransparentBypass over
// the travel so the detour does not stall the world teardown. Game thread.
bool ReturnToMainMenu();

// Load a save slot through the game's own entry (LoadGameFromSlot, setSaveSlotObject with
// loadObjects=true, the game instance's currentLevel named as the game's launch names it, then
// untitled_1; the save selects the mode). True once the level's loadObjects has restored the save
// (ue_wrap/engine/save_restore_watch), not merely once a pawn stands in the level. `forceGameMode` >= 0 writes that
// enum_gamemode ordinal instead of deriving it from the slot-name prefix, which the coop slot
// `zcoop_<pid>` lacks. A slot file in the 0.10.0 compressed format (a host's stale fallback
// streams its world's main.sav as it is) loads through LoadGameCompressed instead. The save is
// registered with no world folder: a joiner never saves. False if the slot is missing or the load
// cannot dispatch. Game thread.
bool LoadStorySave(const wchar_t* slot, int forceGameMode = -1);

// A 0.10.0 world folder's entry ("main" or "autosave"), loaded as the game's save menu loads one:
// the save, the metadata and the persistent object through LoadGameCompressed, all three
// registered with setSaveSlotObject so the game's own saves write back into SaveGames/<folder>,
// the metadata's rules and mode put in force, baseLevel and currentLevel set as the save menu's
// launch sets them (without currentLevel the level's loadObjects restores nothing), then
// untitled_1. InGameplay once the save's restore ran there. Polled like LoadStorySave; Failed is
// final (an entry that does not load, one saved inside an area level, or one whose starter map is
// not untitled_1). Game thread.
enum class LoadResult { Pending, InGameplay, Failed };
LoadResult LoadWorld(const std::wstring& folder, const std::wstring& entry);

// LoadStorySave for a player already standing in a world: a co-op client coming home from a pocket
// level, or from the stale main world the game brought back. LoadStorySave's boot survey answers
// "in gameplay" for any placed pawn of the current world and so never travels from there; this verb
// skips it and otherwise does what LoadStorySave does (the slot loaded from disk once, the
// save-object-ready hook, setSaveSlotObject, the rules, the day, the apply flag, the mode, the
// launch level in force, `open untitled_1`), issuing the open once from the world it is first polled
// in (re-issued at most twice if that world still stands 10 s later). InGameplay once a DIFFERENT
// world of the gameplay kind holds a placed pawn and its save's restore ran there; Failed when the
// slot is missing. Call ResetCachedSave first: it starts the campaign. Polled; game thread.
LoadResult LoadStorySaveForReturn(const wchar_t* slot, int forceGameMode);

// Drop the cached USaveGame* and the GameMode latch so a second in-process LoadStorySave re-loads
// from disk. Game thread.
void ResetCachedSave();

// A fresh New Game in `gameMode` (an enum_gamemode ordinal): LoadStorySave with a blank save
// object (CreateSaveGameObject(saveSlot_C)); a fresh client world holds only level-default props,
// so the host's snapshot mirrors onto it with nothing to reconcile away. The blank save has no
// slot file, so the mode is named rather than derived from a prefix. Game thread.
bool StartFreshGame(int gameMode);

// The rules the next StartFreshGame writes into its blank save's localGameRules before the save's
// rules are put in force: (game_rules key, value text) pairs, as game_rules::WriteSavedRule takes
// them. Consumed by the one fresh save that call creates, then cleared; a save loaded from a slot
// is never touched. Set before the call, from the thread that drives the boot; empty clears.
void SetFreshGameRules(const std::vector<std::pair<std::string, std::string>>& rules);

// ---- Save-object-ready hook ----
// Fires once per LoadStorySave / StartFreshGame, on the game thread, with the USaveGame* about to
// be registered: the inventory arrays are present and the world not yet built from them, so the
// game's own load builds the live inventory from what the hook leaves. engine.cpp fires it
// unconditionally; the hook is a no-op unless an inventory is pending. Null disarms.
using SaveObjectReadyHook = void(*)(void* saveSlotObject);

void SetSaveObjectReadyHook(SaveObjectReadyHook hook);

// The game encodes a save's mode only in the slot-name prefix (story "s_", infinite "i_", sandbox
// "b_", halloween "SPOOKY_", ambience "a_", solar "l_"); both functions wrap
// Uui_saveSlots_C::getSavePrefix on the CDO, which resolves lazily (false / -1 until the menu or a
// gameplay transition loads it). enum_gamemode ordinals are not the submenu order (story=0,
// infinite=1, sandbox=4). Game thread.

// The prefix for `mode` (a TEnumAsByte<enum_gamemode::Type>) into `out`.
bool GetSavePrefix(uint8_t mode, std::wstring& out);

// The ordinal of `slot` from its prefix (the longest match wins): 0..7, or -1.
int DeriveModeFromSlot(const wchar_t* slot);

}  // namespace ue_wrap::engine
