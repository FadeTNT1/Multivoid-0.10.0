// ue_wrap/devices/asologo_piece.cpp -- see ue_wrap/devices/asologo_piece.h.

#include "ue_wrap/devices/asologo_piece.h"

#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

namespace ue_wrap::asologo_piece {

namespace R = reflection;
namespace N = profile::name;

bool IsPiece(void* obj) {
    if (!obj || !R::IsLive(obj)) return false;
    void* base = object_index::ClassByName(N::AsologoPieceClass);
    void* cls = R::ClassOf(obj);
    return base && cls && R::IsDescendantOfAny(cls, &base, 1);
}

bool CallBreak(void* piece) {
    return IsPiece(piece) && component_calls::CallParamlessNamed(piece, N::AsologoPieceBreakFn);
}

}  // namespace ue_wrap::asologo_piece
