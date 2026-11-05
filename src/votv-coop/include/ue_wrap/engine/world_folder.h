// ue_wrap/engine/world_folder.h -- the game's 0.10.0 world folders, driven through the game's own
// functions.
//
// Engine-wrapper layer (principle 7): no co-op state. A world is SaveGames/<folder>/ with a `main`
// and an `autosave` entry (each a saveSlot_C .sav and a save_metadata_C .meta) and one
// save_persistent_C persistent.sav, every file written by the CompressedSaveLoadLib plugin. The
// game's save menu loads an entry with LoadGameCompressed and registers the three objects with
// mainGameInstance_C::setSaveSlotObject; it creates a world with lib_C::createSaveSlotTreeDirectory.
// This module calls the same functions. A world is named by a key: the folder name for its main
// entry, "<folder>/autosave" for its autosave (a folder name cannot hold a slash).
// GAME THREAD ONLY except the pure key helpers and SaveGamesDir once resolved.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::world_folder {

// <ProjectSavedDir>/SaveGames/, with a trailing slash, through the engine's own
// GetProjectSavedDirectory so -saveddirsuffix is honoured. Resolved on the game thread the first
// time; empty until then, after that any thread.
std::wstring SaveGamesDir();

// The key helpers. A key without a slash is the main entry.
std::wstring MakeKey(const std::wstring& folder, const std::wstring& entry);
bool SplitKey(const std::wstring& key, std::wstring& folder, std::wstring& entry);

// True when SaveGames/<folder> exists as a directory. Any thread.
bool Exists(const std::wstring& folder);

// LoadGameCompressed(name, dir, ext): the object the file holds, or null. `dir` is an absolute
// folder path. The object is unrooted: register it before the next garbage collection.
void* LoadCompressed(const std::wstring& name, const std::wstring& dir, const std::wstring& ext);

// The three objects of one entry, loaded as the game's save menu loads them.
struct Entry {
    void* save = nullptr;        // saveSlot_C
    void* metadata = nullptr;    // save_metadata_C
    void* persistent = nullptr;  // save_persistent_C
};
bool LoadEntry(const std::wstring& folder, const std::wstring& entry, Entry& out);

// The world's mode off its metadata (gamemode_type), or -1.
int MetadataGameMode(void* metadata);
// The entry's area, non-empty when it was saved inside an area level.
std::wstring MetadataArea(void* metadata);
// The level the world starts in (save_metadata_C.starterMap), or empty when it is None or does not
// resolve; the game's own launch reads an empty one as untitled_1.
std::wstring MetadataStarterMap(void* metadata);

// The file a saveSlot_C's saveToSlot writes: <savepath>/<slotName>.sav, its own folder stamped at
// creation (a relative one is taken under SaveGamesDir). Empty when either field is empty or does not
// resolve. Game thread.
std::wstring SaveObjectFile(void* saveSlot);

// Put the metadata's rules in force: copied over mainGameInstance.gameRules, which the game's own
// launch does, and over the save object's localGameRules, the copy a joiner's transferred save
// carries and a few blueprints read. Returns how many rules changed on the game instance, -1 when
// a struct did not resolve.
int ApplyMetadataRules(void* gameInstance, void* metadata, void* save);

// Create a world as the game's save menu does, in `mode` (an enum_gamemode ordinal, which the
// game copies off the game instance into the metadata): createSaveSlotTreeDirectory(name), then
// the new main save and metadata saved. Refuses a name that exists or is empty after the game's
// file-name rules. `outFolder` is the folder made.
bool Create(const std::wstring& name, uint8_t mode, std::wstring& outFolder);

// The same from a base name nobody typed: "<base>", "<base> 2", ... the first free one.
bool CreateUnique(const std::wstring& baseName, uint8_t mode, std::wstring& outFolder);

}  // namespace ue_wrap::world_folder
