// ue_wrap/world/achievements.h -- the game's two completion stores, read and driven through its own verbs.
//
// An achievement is a row of the player's profile (mainGamemode.save_main, a save_main_C, written to data10.sav);
// an advancement is a row of the world's save slot (saveSlot_C, in the world's own save, game mode 0 only). Each
// store keeps three parallel arrays (the row names, a struct_achievement per row and a completed flag) and
// appends a row the first time it progresses; a native progress adds one to the row, and the one that reaches its
// progressTarget marks it completed and pays its `award` into the world's balance. A row exists only if the
// game's own catalogue (the cooked list_achievements / list_advancements data table) lists it.
//
// This module reads which rows are completed and completes a row through the native path, so the popup, the
// stored arrays and the laptop list are the game's own. Engine-wrapper layer (principle 7): no network or
// gameplay policy -- coop/world/achievement_sync decides which rows to complete and whether the award is paid.
// Game thread only.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::achievements {

enum class Domain : uint8_t { Achievement = 0, Advancement = 1 };

// The longest row name the wire and this module carry, in characters. A row whose name is longer, or is not
// printable ASCII, is out of reach: it is skipped, said once.
inline constexpr size_t kNameCap = 39;

// Every completed row of `domain`'s store, as ASCII names, in storage order. False, `out` empty, while the store
// or one of its arrays does not resolve (a menu, a world still loading, a build that renamed a member).
bool ReadCompleted(Domain domain, std::vector<std::string>& out);

// Whether the game's catalogue lists the row: 1 yes, 0 no, -1 the table is not loaded yet, -2 its layout does
// not resolve (said once). Case-insensitive, as the game's name compare is.
int InCatalogue(Domain domain, const std::string& name);

enum class Applied : uint8_t {
    Completed,    // the native path ran and the row now reads completed
    AlreadyDone,  // the row was completed before the call; nothing ran
    NotReady,     // the store, its arrays, the catalogue or the game mode is not up yet: ask again later
    Unknown,      // the catalogue does not list the row
    Unsupported,  // the domain does not exist in this world (advancements outside game mode 0)
    Failed,       // the native call did not run, or ran and left the row not completed
};

const char* Describe(Applied result);

// Complete one row through the game's own progress verb, with its popup when `popup` is set. A counter's stored
// progress is first raised to one short of its target, keeping any higher value, so the single native call that
// follows is the one that completes it. The native completion pays the row's own `award` into the balance through
// the game's addPoints: a caller that must not pay holds the script gate's cancel on that function around this call.
Applied Complete(Domain domain, const std::string& name, bool popup);

}  // namespace ue_wrap::achievements
