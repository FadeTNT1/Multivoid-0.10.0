// ue_wrap/actors/save_rows.h -- the world rows of the live save object, read through a MEASURED
// element layout.
//
// The save object holds the world as arrays of per-actor rows: 0.9.0n's objectsData
// (Fstruct_save, stride 0x100) and primitivesData (Fstruct_primitiveSave, stride 0x60); 0.10.0's
// betterObjectsData and betterPrimitivesData (both struct_betterSave, 176 bytes, whose class is a
// TSoftClassPtr rather than a UClass pointer). A reader that walks one of them with a stride and
// member offsets written for another build reads every row after the first from the wrong bytes,
// and dereferences what it finds there as a UClass. So each array's element stride is its inner
// property's ElementSize, each member is found by its stable name prefix and size-checked, and the
// class member is read as either a hard pointer (8 bytes) or a soft one (FSoftObjectPtr), resolved
// to a LOADED class only. An array whose element does not measure that way is refused whole.
//
// Engine-wrapper layer; no gameplay. Game thread.

#pragma once

#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

#include <cstdint>

namespace ue_wrap::save_rows {

// One row as the identity readers need it.
struct RowView {
    void* cls = nullptr;            // the row's UClass, null when unset or not loaded
    ue_wrap::FVector location{};    // the row transform's translation
    reflection::FName key{};        // the row's save key (NAME_None when keyless)
};

// The visitor a walk calls per row, in array order. `index` is the row's position in the array.
using RowFn = void (*)(void* ctx, int32_t index, const RowView& row);

// Walk `saveSlot.<propName>` row by row. Returns the row count walked, 0 for an empty array, or -1
// when the save object has no such array or its element does not measure as a row (said once per
// array name). Pure field reads; no dispatch.
int32_t ForEachRow(void* saveSlot, const wchar_t* propName, RowFn fn, void* ctx);

// Read the class a TSoftClassPtr names at `softPtr` (an FSoftObjectPtr): its weak pointer when that
// still resolves, else its asset path's leaf ("/Game/x/prop_y.prop_y_C" -> "prop_y_C") through the
// object index -- a loaded class only, never a load. Null for an unset or unloaded class.
void* ResolveSoftClass(const uint8_t* softPtr);

}  // namespace ue_wrap::save_rows
