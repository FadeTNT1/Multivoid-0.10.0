// ue_wrap/devices/asologo_piece.h -- engine access for the prop_asologoPiece_C family (radiator_C, radiator_base2_C,
// securityTV_C), the pryables a crowbar tears off the wall. Principle-7 engine-wrapper layer: no network or coop state.
//
// A piece's `break()` is the cosmetic half of its breaking sequence: it activates the break particles and the tear-off
// sound and touches nothing else. CallBreak runs that one verb; CallBreakingSequence runs the whole sequence, which also
// frees the body and breaks its world constraints. The names are in ue_wrap/core/sdk_profile_names.h.

#pragma once

namespace ue_wrap::asologo_piece {

// True iff `obj` is a live prop_asologoPiece_C (or a class below it). Game thread.
bool IsPiece(void* obj);

// The piece's own break(): its break particles and tear-off sound, nothing else. False when `obj` is no piece or the
// function did not dispatch. Game thread.
bool CallBreak(void* piece);

// The piece's whole breakingSequence: frees it from Static, runs break(), then breaks its world constraints one by one
// (a latent chain). The only verb that opens a pried or smashed wall piece's hold on the world. False when `obj` is no
// piece or the function did not dispatch. Game thread.
bool CallBreakingSequence(void* piece);

// What BreakIntactConstraints found on one piece.
struct ConstraintReport {
    int found = 0;    // PhysicsConstraint components the piece owns
    int intact = 0;   // of those, the ones still holding (IsBroken false, or unreadable) that were just broken
};

// Breaks every world constraint of the piece that is still intact, at once and without the sequence's latent chain,
// its spin, its particles or its sound. Idempotent: a constraint already broken is left alone. This is how a copy that
// the host's state says is broken, and whose constraints still hold it, lets go without a restart of the randomised
// sequence. Walks the object array once (reflection::ChildObjectsOf), so for an edge, not a tick. Game thread.
ConstraintReport BreakIntactConstraints(void* piece);

}  // namespace ue_wrap::asologo_piece
