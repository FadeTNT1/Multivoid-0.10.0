// ue_wrap/core/script_stack.h -- the Blueprint call chain a native seam stands in, read without a dispatch.
//
// Two halves. The VM frames above a native call: each FFrame names its function (Node) and the object
// running it, and links to its caller's frame (PreviousFrame). And the ProcessEvent dispatches this thread
// has open, each an {object, function} pair the detour pushes on entry: a ProcessEvent starts a frame chain
// of its own (its frame has no PreviousFrame), so the input event, the latent resume or the widget callback
// behind a spawn is reached only through them. Engine-wrapper layer (principle 7): what a pair means is the
// caller's predicate. The walk is game-thread work; the dispatch stack is kept per thread.

#pragma once

#include <cstdint>

namespace ue_wrap::script_stack {

// One frame or one dispatch: the object running it and its UFunction.
struct Entry {
    void* object;
    void* function;
};

// A frame chain past this many links is cut where it stands, which also stops a corrupted link from looping.
inline constexpr int kMaxFrames = 64;
// Dispatches nested deeper than this are counted but not kept; the outermost ones are the ones kept.
inline constexpr int kMaxDispatches = 64;

// The frame chain from `frame` (a raw FFrame: ufunction_hook::CallerFrame::stack) outward over
// PreviousFrame, innermost first, into out[0..cap). Returns how many were written: 0 for a null frame, and a
// link whose read faults ends the walk there. Field reads only, under SEH; no dispatch.
int WalkFrames(const void* frame, Entry* out, int cap);

// This thread's open ProcessEvent dispatches, innermost first, into out[0..cap). Returns how many were
// written. Past kMaxDispatches the innermost are the ones not kept.
int OpenDispatches(Entry* out, int cap);

// The outermost open dispatch on this thread: the engine's own entry into the VM (an input event, a tick, a
// timer or latent resume, a widget event). {null, null} when none is open.
Entry OutermostDispatch();

// A number naming the outermost dispatch open on this thread: the same for everything nested in it, a new
// one for the next. 0 while none is open.
uint64_t OutermostDispatchSerial();

namespace detail {
// The detour's own scope calls these around each dispatch (ue_wrap/core/pe_detour.cpp). Trivial: they
// cannot fault, so the depth they keep cannot drift.
void PushDispatch(void* object, void* function);
void PopDispatch();
}  // namespace detail

}  // namespace ue_wrap::script_stack
