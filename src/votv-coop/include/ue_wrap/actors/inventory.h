// ue_wrap/actors/inventory.h -- read/write the player-scoped inventory off the live UsaveSlot_C.
//
// Engine-wrapper layer: it owns the saveSlot reflection and the Fstruct_save /
// Fstruct_equipment struct walk, measured off the running classes. This is 0.9.0n's layout, read
// by the dev drills and the save-object resolve; 0.10.0 keeps the player's items on the player's
// own inventory component (ue_wrap/actors/inventory_component), which the profile lane uses.
//
// An inventory item is DATA, not an actor -- an Fstruct_save record (class, transform, key,
// typed key/value groups) -- so reading one means walking the nested TArrays. The value-group
// arrays are TArray<Fstruct_mX> where each Fstruct_mX wraps a single TArray<X>, giving a
// vector of vectors, except `signals`, which is a TArray<Fstruct_signalDataDynamic> directly.
// FNames and UClasses are read as STRINGS: as pointers they do not carry across peers, so the
// apply re-interns them.

#pragma once

#include "ue_wrap/actors/save_record.h"  // SaveRecord + its engine codec (the shared Fstruct_save home)

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::inventory {

// One Fstruct_equipment record (a propDynamic id + an embedded Fstruct_save + a tag).
struct EquipRecord {
    std::wstring propName;  // Fstruct_propDynamic.name FName
    std::wstring propKey;   // Fstruct_propDynamic.key FName
    ue_wrap::save_record::SaveRecord data;  // embedded Fstruct_save (occupies a 0x100 slot)
    std::wstring tag;       // FName
};

// The full player-scoped inventory snapshot.
struct PlayerInventory {
    std::vector<ue_wrap::save_record::SaveRecord> inventory;  // carried: saveSlot.GObjStack[0]
    std::vector<EquipRecord> equipment;  // saveSlot.equipment (worn)
    std::vector<EquipRecord> hold;       // saveSlot.hold (hands)
};

// Resolve the live UsaveSlot_C (via mainGamemode_C.saveSlot). Cached + IsLive-revalidated;
// re-walks on a level transition. null if not yet resolvable. Game thread.
void* ResolveSaveSlot();

// Read what the local player carries, wears and holds into `out` (cleared first): the live
// personal store below, plus saveSlot.equipment and saveSlot.hold, all three of which gameplay
// reads and writes in place. False before the world is up (no player container yet). Pure field
// reads plus FName::ToString, no UFunction dispatch. Game thread.
//
// saveSlot.inventoryData is deliberately not read: it is a projection mainGamemode::saveObjects
// writes and only the save-slot menu's repair routine reads, and on a client it never refreshes.
bool ReadAll(PlayerInventory& out);

// ---- THE LIVE PERSONAL STORE (read-only) ------------------------------------------------------
//
// What the player is ACTUALLY carrying right now, as the game maintains it during play:
//   mainGamemode.playerContainer -> .propInventory -> saveSlot.GObjStack[.Index]
//
// The player's inventory IS a container (Aprop_inventoryContainer_player_C derives from
// Aprop_container_C), so its contents live in the ONE global saveSlot.GObjStack addressed by
// propInventory.Index, exactly like a world container's. That slot is BAKED AT CONSTRUCTION
// rather than restored: the class's component template serializes index=0, player=true,
// customVolume=50000, so GObjStack[0] IS the player's inventory -- which is why the class's
// loadData override is an empty stub. Both live write paths land here: the world pickup
// (mainPlayer::putObjectInventory2 -> playerContainer.propInventory.addObject) and the
// container slot press (getObject -> addObject).
struct LivePersonalStore {
    int32_t slotIndex = -1;  // propInventory.Index -- measured to be 0 by construction
    std::vector<ue_wrap::save_record::SaveRecord> records;  // the live contents
};

// Read the live personal store into `out` (cleared first). False, with `out` left empty, if
// the gamemode, playerContainer, component or GObjStack slot is unresolvable, the index is
// out of range, or the component is not flagged personal. Pure field reads, no UFunction
// dispatch. Game thread.
//
// `Player == true` is the ADDRESS ASSERTION here, and the same flag is a REFUSAL in
// coop/props/container_contents_sync -- deliberately, from opposite sides of one boundary:
// that lane must never author a personal store, and this reader must never read anything
// else. ReadAll goes through this; coop/dev/live_store_readout prints the same records by
// content.
bool ReadLivePersonalStore(LivePersonalStore& out);

// How many records the live personal store holds RIGHT NOW, without decoding any of them: the
// slot's TArray count alone. -1 when the save object or the slot is unresolvable.
//
// It exists because ReadLivePersonalStore interns every FName into a string, which is far too much
// to run from inside a Blueprint body -- and "how many records were in the store at the instant
// the game refreshed the quick-slot bar" is a question that can only be asked from in there. Pure
// field reads. Game thread.
int32_t LivePersonalStoreCount();

// Writes the live save slot's first hold slot, the hand, for a dev drill that takes an item into the
// hand: the item's name (list_props' row), and in its record the class the player's updateHold
// spawns as the hand item and the name that item takes as it loads ("None" and a null class empty
// the hand). False when the save slot or its hold slot cannot be reached, or `classLeaf` names no
// loaded class. Game thread.
bool WriteHeldItem(const std::wstring& name, const wchar_t* classLeaf);

// The item name in the live save slot's first hold slot, the hand: the name the player's updateHold
// keys the game's item tables by (list_props, list_weapons). Empty when the hand holds nothing or the
// slot cannot be read. Game thread.
std::wstring ReadHeldItemName();

}  // namespace ue_wrap::inventory
