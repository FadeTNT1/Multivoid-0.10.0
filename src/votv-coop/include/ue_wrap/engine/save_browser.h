// ue_wrap/engine/save_browser.h -- the worlds a host can pick, listed off disk. Engine-wrapper layer
// (principle 7): NO coop, network or gameplay state lives here.
//
// A 0.10.0 world is a folder under SaveGames (ue_wrap/engine/world_folder): each folder with a
// main.meta is one row, and its autosave a second row once the game has written one. A row's
// numbers come from the entry's .meta (ue_wrap/engine/world_meta), read on a worker and cached by
// modification time, so listing never deserialises a world on the game thread. A pre-0.10.0 flat
// slot (SaveGames/<slot>.sav, GVAS) is listed as legacy: the game converts those in its own Load
// menu, and a world loaded from one would save to the wrong place, so a legacy row is not hostable.
// Which flat slots are the game's sub-saves is decided by the game's own classifier.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::save_browser {

struct SaveInfo {
    std::wstring world;        // the key (world_folder::MakeKey), or the slot name of a legacy row
    std::wstring folder;       // the world folder; empty for a legacy row
    std::wstring entry;        // "main" or "autosave"; empty for a legacy row
    bool         legacy = false;   // a pre-0.10.0 flat slot: shown, not hostable
    bool         inArea = false;   // the entry was saved inside an area level: shown, not hostable
    std::wstring displayName;  // the folder (with " (autosave)"), or the slot minus its mode prefix
    int          mode = -1;    // enum_gamemode ordinal; -1 = unknown
    std::wstring modeLabel;    // "Story" / "Sandbox" / ... (display)
    int          day = 0;      // savedtime.Z + 1, as the game's row prints it
    std::wstring version;      // the game version the entry was last saved by
    bool         versionConflict = false;  // differs from the running game's (GameVersion)
    int64_t      lastPlayedTicks = 0;      // FDateTime ticks (100 ns)

    bool Hostable() const { return !legacy && !inArea; }
};

// Enumerate synchronously (a dev probe; the picker uses RefreshAsync). Sorted newest-first. False
// when the save system cannot be resolved yet. Game thread.
bool EnumerateSaves(std::vector<SaveInfo>& out);

// Kick a scan: the directory listing and the game's defaults on the game thread, the .meta reads
// on a worker. Non-blocking, coalesced, safe from the render thread.
void RefreshAsync();

// Copy the cached list. Returns a revision counter that bumps on each completed scan.
uint64_t CopySaves(std::vector<SaveInfo>& out);

// The running game's version as its own slot menu compares it (lib_C::gameVersion). Resolved by a
// scan on the game thread; empty until one has. Any thread.
std::wstring GameVersion();

// One-line status for the picker footer ("Scanning..." / "N worlds" / an error).
std::string Status();

}  // namespace ue_wrap::save_browser
