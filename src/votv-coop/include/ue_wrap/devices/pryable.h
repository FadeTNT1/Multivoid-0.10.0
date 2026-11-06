// ue_wrap/devices/pryable.h -- engine access for a pryable's prying crowbar (pryingCrowbar_C), the child actor every
// pryable carries hidden until a crowbar goes into it. Principle-7 engine-wrapper layer: no network or coop state.
//
// The child is the whole pry: its begin() takes the held crowbar and reveals itself stuck in the parent, the player's
// mouse raises its `alpha` and `snapStress`, its end() gives the crowbar back and, on a pry, runs the parent's
// crowbarOpen. This wrapper reads that state for a sender and poses a copy of it for a receiver, without running the
// begin or the player's input on the copy. The names are in ue_wrap/core/sdk_profile_names.h.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstddef>
#include <cstdint>

namespace ue_wrap::pryable {

// True iff `obj` is a live pryingCrowbar_C (or a class below it). Game thread.
bool IsPryingCrowbar(void* obj);

// Every live prying crowbar of the running world, handed to `fn`. Walks the class's instances in the object index,
// never the object array. Game thread; `fn` must not create or destroy objects.
using ChildFn = void (*)(void* ctx, void* child);
size_t ForEachPryingCrowbar(ChildFn fn, void* ctx);

// The pryable the child is stuck in: its parent actor, or null.
void* ParentOf(void* child);

// What a sender reads and a receiver shows. `pivot` is sk_pivot's world location, `pitch` its relative pitch, `angle`
// the bend AnimBP's angle.
struct State {
    FVector pivot{};
    float   alpha = 0.f;
    float   stress = 0.f;
    float   pitch = 0.f;
    float   angle = 0.f;
    uint8_t lives = 0;
    bool    stuck = false;      // the child is shown: a crowbar is in it
    bool    working = false;    // a player's mouse drives it (its lastPlayer set and its UI open)
    bool    straining = false;
    bool    strainSound = false;
    bool    sparked = false;
};

// Read the child's state. False for anything that is not a prying crowbar or a member that did not resolve.
bool Read(void* child, State& out);

// Receiver: show the child stuck, as a crowbar in it looks, on a copy no local player drives: shown with its own tick
// and collision off, sk_pivot at `pivot`, its lives, and the bend mesh built once (the game's createSkeletalMesh),
// then the stick sound. The begin is not run: it takes the local player's held crowbar.
bool ShowStuck(void* child, const FVector& pivot, uint8_t lives);

// Receiver: the bend as the sender had it -- alpha, stress, sk_pivot's pitch and the AnimBP's angle -- and the strain
// and spark the sender's state turned on since `prev`.
bool ShowProgress(void* child, const State& now, const State& prev);

// The game's own end(pryOpen, dropSelf, destroyed) on the child. `lastPlayer` is written to the child's lastPlayer
// first (null on a copy no local player drove: its release then touches no player's input), and with `keepUi`
// false its UI pointer is cleared, so a copy's end never closes the local player's own pry screen.
bool CallEnd(void* child, bool pryOpen, bool dropSelf, bool destroyed, void* lastPlayer, bool keepUi);

// The game's own snap() with `lives` written to currentLives first: one life less, a new crowbar or its gibs, then
// the end.
bool CallSnap(void* child, uint8_t lives);

// The parameter offsets of end() as its body's frame lays them out, for a script-gate callback. -1 when they did not
// resolve.
struct EndParams { int32_t pryOpen = -1, dropSelf = -1, destroyed = -1; };
EndParams EndParamsOf(void* endFunction);

}  // namespace ue_wrap::pryable
