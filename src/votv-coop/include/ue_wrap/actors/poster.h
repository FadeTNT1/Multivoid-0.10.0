// ue_wrap/actors/poster.h -- engine access for the poster, rolled (prop_poster_C, a prop) and placed (poster_C, an
// actor_save_C with a decal). Principle-7 engine-wrapper layer: no network or coop state. What the two Blueprints do
// is in ue_wrap/core/sdk_profile_names.h, with the names.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::poster {

// True iff `obj` is a live actor of the rolled (prop_poster_C) or placed (poster_C) family, the class or below.
bool IsRolled(void* obj);
bool IsPlaced(void* obj);
// The same of a class.
bool IsRolledClass(void* cls);
bool IsPlacedClass(void* cls);

// What a poster shows: the preset `type`, the placed form's `index`, the custom image's `fileName` and `custom`.
struct Look {
    int32_t      type = 0;
    int32_t      index = 0;
    std::wstring fileName;   // "None" for none
    bool         custom = false;
};
bool operator==(const Look& a, const Look& b);
inline bool operator!=(const Look& a, const Look& b) { return !(a == b); }

// Read the look of either form. False for anything else or a member that did not resolve.
bool ReadLook(void* poster, Look& out);

// Write `fileName` alone (the picker's write), no redraw. False as above or when the name cannot be made.
bool WriteFileName(void* poster, const std::wstring& fileName);

// Write a placed poster's whole look (type, index, fileName, custom), no redraw. False for anything but a placed
// poster, a member that did not resolve, or a name that cannot be made.
bool WriteLook(void* placed, const Look& look);

// The form's own redraw: setTex on the rolled form, setPoster on the placed. A `custom` poster whose `fileName` is not
// among this peer's custom poster files is drawn with its preset `type` instead: `custom` is cleared for the call
// and put back, so the state is untouched and only the picture falls back (`fellBack`). False when the function did
// not resolve or the call failed.
bool Redraw(void* poster, bool* fellBack = nullptr);

// The class the form turns into (`posterClass`): the placed class of a rolled poster, the rolled class of a placed
// one. Null when unset or unresolved.
void* PosterClassOf(void* poster);

// The rolled poster's holder (`holdingPlayer`), or null.
void* HoldingPlayer(void* rolled);

// Where put() would place the rolled poster: the `turn` component's world location, rotation and scale. False when
// the component or a read did not resolve, or a value is not finite.
struct Placement {
    FVector  location{};
    FRotator rotation{};
    FVector  scale{1.f, 1.f, 1.f};
};
bool ReadPlacement(void* rolled, Placement& out);

// Run the rolled poster's own put() (the game's native placement and consumption of the held poster). False for
// anything but a rolled poster, or when the function did not resolve or the call failed; whether it placed anything is
// the caller's to see (it places nothing without a blocking hit). Game thread.
bool CallPut(void* rolled);

// A placed poster's transform: its actor location, rotation and root scale. False on a failed read.
bool ReadTransform(void* placed, Placement& out);

// The placed poster's actor_save key, "" when it has none; and its write. Game thread.
std::wstring KeyOf(void* placed);
bool SetKey(void* placed, const std::wstring& key);

// Spawn a placed poster of `cls` at `at`, as put() does: `type`, `fileName` and `custom` written in the deferred
// window (and `index`, and the key, when `writeIndex` / `key` non-empty), the root scale set after the finish, then
// the redraw. Null on any failure.
void* SpawnPlaced(void* cls, const Placement& at, const Look& look, bool writeIndex, const std::wstring& key);

// Spawn a rolled poster of `cls` at `location`/`rotation`, as action 7 does: `type`, `index` and `fileName` written
// in the deferred window. Null on any failure.
void* SpawnRolled(void* cls, const FVector& location, const FRotator& rotation, const Look& look);

// Every live placed poster, of every class of the family. Game thread.
std::vector<void*> AllPlaced();

}  // namespace ue_wrap::poster
