// ue_wrap/actors/inventory_item_codec.h -- one 0.10.0 inventory row (struct_inventoryItem) as bytes, through the
// struct's own reflection.
//
// A row is data, not an actor: a place on the canvas, a rotation, an amount, a compartment, and a struct_betterSave
// holding the item's class (a soft path), its names and key, its transform and its own serialized state
// (actorData). The plan of the struct is measured off the running build once -- every member's offset, size and
// property type, recursively -- and a row is written as those values in plan order: no pointer, no object
// reference, nothing a receiver cannot rebuild by name. A member of a type the plan does not carry (an object, a
// text, a map) refuses the whole plan, said once, and every call below then answers false.
//
// Decode builds every array, string and name in engine-owned memory (GMalloc, interned names) inside memory the
// caller zeroed and the engine owns (an array element of a component), so the engine destroys what it built, as it
// destroys anything of its own. Every count and length read off the wire is bounded before any allocation.
// Engine-wrapper layer (principle 7): no wire framing and no gameplay. Game thread.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::inventory_item_codec {

// The longest encoded row accepted on either side, and the longest name or string inside one.
inline constexpr size_t kMaxItemBytes = 48 * 1024;
inline constexpr size_t kMaxStringChars = 512;

// Whether the plan is measured: inventoryComponent_C is loaded and its canvas row reads as a tree of carried types.
// Asked again at most once a second while it is not. `stride` is the canvas array's element size, `signature` a hash
// of the plan (member names, kinds and sizes) that two peers compare before either decodes the other's rows.
bool Ready();
int32_t RowStride();
uint32_t PlanSignature();

enum class Mode : uint8_t {
    Full,      // every member: what a receiver rebuilds the row from
    Identity,  // the place, the rotation and the amount left out: what the item IS, so two peers name it alike
};

// Append the encoding of the row at `row` to `out`. `amountOverride` >= 0 replaces the row's amount (a part of a stack).
// False, `out` as it was, when the plan is not ready, the row does not read or it encodes past kMaxItemBytes.
bool Encode(const uint8_t* row, std::vector<uint8_t>& out, Mode mode = Mode::Full, int amountOverride = -1);

// FNV-1a of the Identity encoding; 0 when the row does not encode.
uint64_t Fingerprint(const uint8_t* row);

// Decode one Full encoding at `b[o..]` into the zeroed row at `row`, advancing `o`. False on a malformed or over-bound
// encoding; what was built before the failure is engine-owned and is destroyed with the row's array.
bool Decode(const std::vector<uint8_t>& b, size_t& o, uint8_t* row);

// A cleared row (removeInventoryItem leaves one in place): no class path and no name.
bool IsEmpty(const uint8_t* row);
uint8_t Amount(const uint8_t* row);
void SetAmount(uint8_t* row, uint8_t amount);
// The item's name (data.name, the row name of the game's item tables); empty when it has none.
std::wstring Name(const uint8_t* row);
// The row's place on its canvas and its rotation.
bool Placement(const uint8_t* row, float& x, float& y, float& rotation);
// Write the row's place on its canvas and its rotation.
bool SetPlacement(uint8_t* row, float x, float y, float rotation);

}  // namespace ue_wrap::inventory_item_codec
