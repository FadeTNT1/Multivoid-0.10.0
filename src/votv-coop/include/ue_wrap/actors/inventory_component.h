// ue_wrap/actors/inventory_component.h -- the player's 0.10.0 inventory, through the game's own codec.
//
// 0.10.0 keeps everything a player carries, holds and wears on one component,
// mainPlayer_C.inventoryComponent_player: the carried grid (canvasInventory), the hands and hotbar
// (SlotArray_A) and the equipment slots (SlotArray_B), each an array of struct_inventoryItem whose
// data member is a whole struct_betterSave. The game saves it as ThingsAndStuffLib::ObjectToBytes
// of the component (mainPlayer_C.betterGetData) and loads it with BytesIntoObject
// (betterLoadData). This wrapper uses the same two natives, so a profile is exactly what the
// game's own save holds for the player, item state (a flashlight's battery) included, and nothing
// here knows an element's layout. Engine-wrapper layer: no gameplay or network logic. Game thread.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::inventory_component {

// The player's inventoryComponent_player, or null.
void* Of(void* player);

// The component as the game's own save serializes it. False when the component or the natives do
// not resolve, and when the native hands back zero bytes: that is a failed capture, not an
// inventory that holds nothing, so a caller never publishes it over a stored profile.
bool Capture(void* player, std::vector<uint8_t>& out);

// Replace what the player carries, holds and wears with `bytes` (a Capture): the three item arrays
// are emptied first, because a tagged-property load leaves a property the bytes do not carry as it
// was, and an empty array is exactly what a serializer leaves out. Empty `bytes` only empties.
// The emptied elements are orphaned rather than destroyed, a bounded few kilobytes once per join:
// their nested strings and arrays belong to the engine's allocator, and the load that follows
// builds fresh elements. False, having written nothing, when an array or a native does not resolve.
bool Restore(void* player, const std::vector<uint8_t>& bytes);

// The same two operations on any inventoryComponent_player_C, not only a player's: the capture of its bytes, and a
// load that first empties the three item arrays, exactly as Capture and Restore do for the player's own.
bool CaptureComponent(void* component, std::vector<uint8_t>& out);
bool LoadComponent(void* component, const std::vector<uint8_t>& bytes);

// A scratch inventoryComponent_player_C owned by nobody (its outer is the game instance, never an actor), to read or
// rebuild a stored profile's items off any player: the host's sleepers load a profile into one, read its rows and
// write the rows that are left back. The collector takes it once the calling task has returned. Null when the class
// or the outer does not resolve.
void* NewScratchComponent();

// How many items each array holds; -1 when it does not resolve.
struct Counts {
    int32_t carried = -1;    // canvasInventory
    int32_t hands = -1;      // SlotArray_A
    int32_t equipment = -1;  // SlotArray_B
    int32_t slotC = -1;      // SlotArray_C
    int32_t slotD = -1;      // SlotArray_D
};
Counts CountItems(void* player);

// The item key of the actor in the player's hand (mainPlayer.holding_actor): the row name of the
// game's item tables (list_props, list_weapons) that the hit judge keys a swing by. Read-only; empty
// when `holdingProp` is null, not live, not a prop, or carries no name. Game thread.
std::wstring HeldItemKey(void* holdingProp);

// The player's updateEquipment: rebuilds the equipped flags, the capacity and the flashlight's
// properties from the equipment slots, and broadcasts equipmentUpdated. False when unresolved.
bool CallUpdateEquipment(void* player);

// Arrange for `saveSlot` (a save object no world has been built from) to hand its player the
// game's starting kit: the inventory screen grants saveSlot.startupEquipment on the first frame of
// the world and then clears it, and a save that has been played holds it empty. The map is made by
// the game's own copy of defaultStartupEquipment (reset_player on a scratch save object) and
// swapped in, so no map is ever written byte by byte. False when the save class or the copy does
// not resolve.
bool ArmStartupKit(void* saveSlot);

// The two moments of a world load that touch the player's inventory, watched at the script gate:
// a player's betterLoadData has returned (its component now holds the save's items), and the game
// mode's loadObjects has returned (every saved object is in the world). The callbacks run on the
// game thread inside the load. InstallLoadWatches is idempotent; it returns true once both watches
// are registered, and they go live when the gate resolves their names.
using PlayerLoadedFn  = void (*)(void* player);
using ObjectsLoadedFn = void (*)(void* gamemode);
bool InstallLoadWatches(PlayerLoadedFn onPlayerLoaded, ObjectsLoadedFn onObjectsLoaded);

}  // namespace ue_wrap::inventory_component
