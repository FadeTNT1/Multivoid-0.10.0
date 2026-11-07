// ue_wrap/actors/glowstick.h -- reflected access to prop_glowstick_C's hang: the members its stick leaves behind
// and the game's own verbs that make and break it. Engine layer only; the lane that mirrors it is
// coop/props/glowstick_hang_sync. The mechanism and member names are on ue_wrap/core/sdk_profile_names.h.

#pragma once

#include "ue_wrap/core/types.h"

namespace ue_wrap::glowstick {

// True for a live prop_glowstick_C or a subclass (the coloured sticks). Game thread.
bool IsGlowstick(void* actor);

// The stick's own state, read off the live members. False on a non-glowstick or a member that did not resolve.
// Game thread.
bool ReadSticked(void* actor, bool& out);
bool ReadStickStatic(void* actor, bool& out);
// `loc`: the point the stick hangs from (the impact point of the trace that stuck it).
bool ReadHookPoint(void* actor, FVector& out);

// The `hook` component's world location (K2_GetComponentLocation). False on a non-glowstick, a member that did not
// resolve or a read that did not run. Game thread.
bool ReadHookWorldLocation(void* actor, FVector& out);

// Hang the stick on this peer's copy the way stickto's tail does for a hit on world geometry: detach the `hook`
// component from its parent (as every move of it in the game does), write `sticked`, `stickstatic` and `loc`, put
// `hook` at `loc`, and run the stick's own `hookdelay`, which binds the
// constraint to the stick's mesh and sets its frames. Only a static stick is replayable (a stick on a simulating
// prop needs the hit component). False when a member or the verb did not resolve. Game thread.
bool CallHang(void* actor, const FVector& hookPoint);

// The stick's own `releaseglowstick`: the constraint broken, the hook detached, `sticked` cleared. False when it did
// not resolve. Game thread.
bool CallRelease(void* actor);

}  // namespace ue_wrap::glowstick
