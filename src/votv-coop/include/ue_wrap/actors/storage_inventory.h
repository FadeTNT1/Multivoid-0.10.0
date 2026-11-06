// ue_wrap/actors/storage_inventory.h -- 0.10.0's world storages: the inventoryComponent_C an actor keeps its items
// in (a container prop, the drone, the ATV), read and written through the component's own verbs.
//
// A world storage is a component whose `owner` is a live actor that is not a player, and which is that actor's own
// `inventoryComponent`: the player's inventoryComponent_player_C is never one, whatever its fields say. Its contents
// are the non-empty rows of its canvas array, each carried as an inventory_item_codec row. A whole set of rows is
// applied by decoding them into a scratch component the engine owns and handing them over with the component's own
// transferFromInvToInf, which copies them in and destroys what the storage held before; one row is added with
// addItemAdvanced and removed with removeInventoryItem, so the volumes, the sounds and the open screen follow as the
// game makes them follow. Nothing here is a wire or a rule of who may write. Engine-wrapper layer (principle 7).
// Game thread.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::storage_inventory {

// The actor's inventoryComponent when it is a live inventoryComponent_C, else null.
void* StorageOf(void* actor);
// The component's owner, live, or null.
void* OwnerOf(void* component);
// Fail closed: the component is a world storage (see the file comment). False for anything unresolved.
bool IsWorldStorage(void* component);
// The singleton delivery drone's storage (drone_C.inventoryComponent), or null.
void* DroneStorage();
// Whether `actor` is the delivery drone.
bool IsDrone(void* actor);

// One stored row: its Full encoding, its fingerprint (the Identity encoding's), its amount and its canvas index.
struct Row {
    std::vector<uint8_t> bytes;
    uint64_t fingerprint = 0;
    uint8_t amount = 0;
    int32_t index = -1;
};
// The storage's non-empty canvas rows in canvas order. False when the component or the plan does not resolve, a row
// does not encode, or the canvas holds more than `maxRows`. A slot array (the player's hands and hotbar) that holds a
// row on a world storage is said once and left out: world storages keep their items on the canvas.
bool ReadRows(void* component, std::vector<Row>& out, size_t maxRows);

// Replace the storage's canvas with `rows` (Full encodings). The rows are decoded and checked first, into a scratch
// component; the storage is touched only once every row decoded. Then the screen showing it, if any, is unbound,
// the rows are moved in, the volumes re-derived and the screen bound again.
enum class ApplyResult : uint8_t { Done, Malformed, Unresolved };
ApplyResult ApplyRows(void* component, const std::vector<std::vector<uint8_t>>& rows);

// The canvas index of the first non-empty row with this fingerprint and at least `minAmount`, or -1.
int32_t FindRow(void* component, uint64_t fingerprint, uint8_t minAmount);
// The row at `index`, encoded (Full), with its amount replaced when `amountOverride` >= 0.
bool EncodeRowAt(void* component, int32_t index, std::vector<uint8_t>& out, int amountOverride = -1);
// The component's removeInventoryItem on the canvas row at `index`. False when it did not report success.
bool RemoveRowAt(void* component, int32_t index);
// Take `amount` off the stack at `index` (the row is written in place, as the game's own takeFromStack writes it),
// then the volumes are re-derived. False when the row holds no more than `amount`.
bool ReduceRowAt(void* component, int32_t index, uint8_t amount);

// Add one Full-encoded row through addItemAdvanced: at its own place and rotation when `keepPlace`, else at a random
// place on the canvas. False when the row does not decode or the component refused it (no room, no class).
bool AddRow(void* component, const std::vector<uint8_t>& row, bool keepPlace);

// Decode a Full-encoded row from a peer, checked, without touching any storage: its name, amount and fingerprint.
bool PeekRow(const std::vector<uint8_t>& row, std::wstring& name, uint8_t& amount, uint64_t& fingerprint);

// What a put is judged on: the component's capacity and fill, whether it is infinite, and the owner's `locked`.
struct Terms {
    float maxVolume = 0.f;
    float totalVolume = 0.f;
    bool infinite = false;
    bool locked = false;
};
bool ReadTerms(void* component, Terms& out);

// The local player's own component (inventoryComponent_player) holding a row with this fingerprint and at least
// `amount`: the row is taken off it (removed whole, or reduced) through the same verbs. False when none holds it.
bool TakeFromPlayer(void* playerComponent, uint64_t fingerprint, uint8_t amount);
// Add a Full-encoded row to the player's own component. With the inventory screen open and nothing held, the row is
// kept at the place it carries (where the game's own beginDrag puts a container item it hands to the cursor);
// otherwise it lands at a random free place. `widget` receives the item widget drawn for it, or null. False when the
// component refused it.
bool GiveToPlayer(void* playerComponent, const std::vector<uint8_t>& row, void*& widget);
// How a granted take ended up: held on the cursor (the drag resumed), dropped where the cursor was because the button
// had already gone up, or stored in the inventory with no drag (the screen closed, or it holds something else).
enum class HeldDrag : uint8_t { Held, Dropped, Stored };
// Make the player's `widget` (from GiveToPlayer) the screen's held item, as beginDrag does for a widget of the
// player's own canvas, without splitting its stack again. When the mouse button is already up the drag is ended at once.
HeldDrag ResumeDrag(void* widget);
// The place and rotation a Full-encoded row carries.
bool PeekPlacement(const std::vector<uint8_t>& row, float& x, float& y, float& rotation);

// The inventory screen: the storage it shows beside the player's, or null when it is closed or shows none.
void* ShownStorage();
// Whether `obj` is the inventory screen (a ui_freeLayoutInventory_C): the caller a drag's own verbs name.
bool IsInventoryScreen(void* obj);

// The verbs' frames, read at the script gate's entry (`locals` and `function` are the call's). Each answers false
// when a parameter does not resolve.
// beginDrag(item): the dragged widget's storage and canvas index.
bool BeginDragRow(const uint8_t* locals, void* function, void*& component, int32_t& index);
// The amount a beginDrag takes off a row of `rowAmount` as the screen stands: half with the stack key, one with
// the right button, else the whole row, as the screen's own takeFromStack splits it.
uint8_t BeginDragAmount(uint8_t rowAmount);
// addItemAdvanced(data, ...): the row asked to be added (Full), its fingerprint and amount, and whether it keeps
// its own place. When it does, the encoded row carries the place the call names (itemPosition, itemRotation).
bool AddCallRow(const uint8_t* locals, void* function, std::vector<uint8_t>& row, uint64_t& fingerprint,
                uint8_t& amount, bool& keepPlace);
// transferFromInvToInf(to): the receiving component.
void* TransferCallTarget(const uint8_t* locals, void* function);
// An item widget's own component (takeFromStack and addToStack run on the widget).
void* ItemWidgetComponent(void* itemWidget);
// The component of the row the screen's double-click acts on (doubleClickObject), or null.
void* DoubleClickComponent(void* screen);
// The fingerprint and amount of the canvas row at `index`. False when it is empty or unreadable.
bool RowInfo(void* component, int32_t index, uint64_t& fingerprint, uint8_t& amount);

// ---- a stored player profile's items as storage rows, and back ----
// A player's profile carries the game's own bytes of their inventoryComponent_player (ue_wrap/actors/
// inventory_component). The host's sleepers lay those items out in a world storage for others to loot and write what
// is left back into the profile. Both directions load the bytes into a scratch player component and read or write
// its arrays through the row codec; no item layout is written by hand.

// The rows of `items`: the carried grid's in `canvasRows` (each at its own place), the hands', hotbar's and
// equipment slots' in `slotRows` (their places mean nothing on a grid). A slot array whose element is not a canvas
// row is left out, said once. False when the bytes do not load or a row does not encode.
bool ProfileRows(const std::vector<uint8_t>& items, std::vector<std::vector<uint8_t>>& canvasRows,
                 std::vector<std::vector<uint8_t>>& slotRows);

// `baseItems` with only `rows` (Full encodings, a storage's rows) left in it: a slot item still among the rows stays
// in its slot, with the amount left of it; a slot item no longer there leaves its slot empty; every other row goes
// on the carried grid at the place it has in the storage. The new component bytes in `out`. False, `out` empty, when
// the base does not load, a row does not decode or the capture fails.
struct RebuildCounts {
    int32_t slotsKept = 0;
    int32_t slotsEmptied = 0;
    int32_t carried = 0;
};
// `keepSlots` false puts every row on the carried grid and empties every slot. The split is
// ue_wrap/actors/sleeper_rebuild_plan's, whose invariant (per kind, kept plus carried equals the crate) is checked
// before anything is written.
bool RebuildProfileItems(const std::vector<uint8_t>& baseItems, const std::vector<std::vector<uint8_t>>& rows,
                         bool keepSlots, std::vector<uint8_t>& out, RebuildCounts& counts);

// Whether `items` (a profile's component bytes) hold exactly what `rows` hold: per item kind (fingerprint), the same
// total amount, across the carried grid and every slot. The host's check of a rebuild before it is kept. `itemCount`
// receives how many items (rows) the profile holds. False when they differ or a side does not read.
bool ItemsMatchRows(const std::vector<uint8_t>& items, const std::vector<std::vector<uint8_t>>& rows, size_t* itemCount);

// The component's infiniteVolume bool: a storage that refuses no put for room. False when it does not resolve.
bool SetInfiniteVolume(void* component, bool on);

// Open the local player's inventory screen onto `containerActor` (a prop_container_C) through the gamemode's own
// openPropInv, as the drone and the ATV open theirs. False when the gamemode or the verb does not resolve.
bool OpenScreenOnto(void* containerActor);

}  // namespace ue_wrap::storage_inventory
