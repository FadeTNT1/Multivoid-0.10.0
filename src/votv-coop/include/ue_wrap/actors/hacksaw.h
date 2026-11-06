// ue_wrap/actors/hacksaw.h -- engine access for the hacksaw (prop_hacksaw_C), the hand item that cuts a heavy prop
// into scrap. Principle-7 engine-wrapper layer: no network or coop state.
//
// The saw holds its cut on itself: `sawingProp` (the prop under the cut), `complete` (0..1), `time` (the seconds the
// whole cut takes) and `amount` (the scraps it leaves). Its ReceiveTick raises `complete`; at 1 the tick spawns the
// scraps, breaks and destroys the prop and resets the saw (completeSaw). The names are in
// ue_wrap/core/sdk_profile_names.h.

#pragma once

#include <cstdint>

namespace ue_wrap::hacksaw {

// True iff `obj` is a live prop_hacksaw_C (or a class below it). Game thread.
bool IsHacksaw(void* obj);

// The prop the saw is cutting, or null when it cuts nothing.
void* SawingProp(void* saw);

// The cut's progress (0..1) and its whole time in seconds. False for anything that is not a saw.
bool ReadCut(void* saw, float& complete, float& timeSec);

// Write `complete`. False for anything that is not a saw.
bool WriteComplete(void* saw, float complete);

// The saw's own reset, completeSaw(): its tick off, its prop and progress cleared, its progress bar gone.
bool CallCompleteSaw(void* saw);

// The game's own measure of a cut of `target`: the convex volume of its static mesh and the density of its
// material's physics, as the saw's playerHandUse_LMB takes them. False when the volume or the density cannot be read.
bool MeasureCut(void* target, float& timeSec, int32_t& amount);

// Whether the target answers its own isNotSawable() false. False also for anything that cannot be asked.
bool TargetSawable(void* target);

// Run the cut's end on `saw` for `target`: the saw's own processMat() names the scrap, `amount` and the cut are
// written, and one ReceiveTick of the saw spawns the scraps, breaks and destroys the target and resets the saw.
// False, running nothing, when the saw or the target is not live or processMat refuses the material.
bool RunCutEnd(void* saw, void* target, int32_t amount);

}  // namespace ue_wrap::hacksaw
