// ue_wrap/actors/toolbox.h -- engine access for the repair toolbox (prop_toolbox_C) and the burst water pipe
// (burstWaterPipe_C) it repairs. Principle-7 engine-wrapper layer: reflection and calls, no network or coop state.
//
// The kit, held, takes the looked-at actor as `fixingActor` when that actor can be fixed, reads the actor's fix time
// into `time`, raises `complete` on its tick, and at 1 calls the target's toolboxFix: on true it takes one of `fixes`
// and ends. A pipe holds `damaged`; its state(damaged) sets the flag, the tick, the spray and the sound. The names are
// in ue_wrap/core/sdk_profile_names.h.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ue_wrap::toolbox {

// ---- the kit ------------------------------------------------------------------------------------------------------
// True iff `obj` is a live prop_toolbox_C (or below).
bool IsToolbox(void* obj);

// The actor the kit is repairing, or null.
void* FixingActor(void* kit);

// The repair's progress (0..1), its whole time in seconds and the uses the kit has left. False for a non-kit.
bool ReadFix(void* kit, float& complete, float& timeSec, int32_t& fixes);

// Write `complete`. False for a non-kit.
bool WriteComplete(void* kit, float complete);

// The kit's own reset, completeSaw(): tick off, target and progress cleared, progress bar gone.
bool CallCompleteSaw(void* kit);

// Run the repair's end on the kit: `complete` is set to 1 and one ReceiveTick of zero seconds runs the game's own
// sequence (the target's toolboxFix, a use taken, the reset, the uses message, a destroy when none are left). False
// when the kit is not live or the tick did not run.
bool RunFixEnd(void* kit);

// The kit class's repair sound (its default object's `fixAudio`), or null while the class is not loaded.
void* FixSound();

// ---- the pipe -----------------------------------------------------------------------------------------------------
// True iff `obj` is a live burstWaterPipe_C.
bool IsPipe(void* obj);

// The pipe's `damaged`. False for a non-pipe.
bool PipeDamaged(void* pipe, bool& damaged);

// The pipe's own state(damaged), which sets the flag, the tick, the spray and the sound. False when it did not run.
bool PipeSetState(void* pipe, bool damaged);

// The pipe's toolboxFixTime, seconds. False when it did not run.
bool PipeFixTime(void* pipe, float& timeSec);

// The pipe's toolboxFix(kit): true in `fixed` only when it was damaged and is now mended. `kit` may be null.
bool PipeToolboxFix(void* pipe, void* kit, bool& fixed);

// Up to `cap` takeable burst water pipes of the running world.
size_t Pipes(void** out, size_t cap);

}  // namespace ue_wrap::toolbox
