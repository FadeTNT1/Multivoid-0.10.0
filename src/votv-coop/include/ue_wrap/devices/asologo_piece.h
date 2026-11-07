// ue_wrap/devices/asologo_piece.h -- engine access for the prop_asologoPiece_C family (radiator_C, radiator_base2_C,
// securityTV_C), the pryables a crowbar tears off the wall. Principle-7 engine-wrapper layer: no network or coop state.
//
// A piece's `break()` is the cosmetic half of its breaking sequence: it activates the break particles and the tear-off
// sound and touches nothing else. This wrapper runs that one verb on a copy whose physics the prop lanes already move,
// without the sequence that frees the body and breaks its constraints. The names are in ue_wrap/core/sdk_profile_names.h.

#pragma once

namespace ue_wrap::asologo_piece {

// True iff `obj` is a live prop_asologoPiece_C (or a class below it). Game thread.
bool IsPiece(void* obj);

// The piece's own break(): its break particles and tear-off sound, nothing else. False when `obj` is no piece or the
// function did not dispatch. Game thread.
bool CallBreak(void* piece);

}  // namespace ue_wrap::asologo_piece
