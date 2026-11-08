// ue_wrap/engine/character_hold.cpp -- see ue_wrap/engine/character_hold.h.

#include "ue_wrap/engine/character_hold.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"

#include <cstdint>

namespace ue_wrap::character_hold {
namespace {

namespace P = ue_wrap::profile;
namespace N = ue_wrap::profile::name;
namespace R = ue_wrap::reflection;

// The character's movement component when both are live, else null.
void* LiveMovement(void* character) {
    if (!character || !R::IsLive(character)) return nullptr;
    void* cmc = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(character) +
                                          P::off::ACharacter_CharacterMovement);
    return (cmc && R::IsLive(cmc)) ? cmc : nullptr;
}

uint8_t ModeOf(void* cmc) {
    return *(reinterpret_cast<const uint8_t*>(cmc) + P::off::UCharacterMovement_MovementMode);
}

// SetMovementMode(mode, 0) through the engine's own verb, so the transition runs.
bool SetMode(void* cmc, uint8_t mode) {
    void* fn = R::FindDispatchFunction(R::ClassOf(cmc), N::CharacterSetMovementModeFn, nullptr);
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<uint8_t>(N::CharacterSetMovementModeParam, mode);
    f.Set<uint8_t>(N::CharacterSetCustomModeParam, uint8_t{0});
    return ue_wrap::Call(cmc, f);
}

}  // namespace

Hold HoldStill(void* character) {
    void* cmc = LiveMovement(character);
    if (!cmc) return Hold::Unresolved;
    const uint8_t mode = ModeOf(cmc);
    if (mode == P::off::kMOVE_None) return Hold::Holding;
    if (mode == P::off::kMOVE_Falling) return Hold::Airborne;
    return SetMode(cmc, P::off::kMOVE_None) ? Hold::Holding : Hold::Unresolved;
}

bool Release(void* character) {
    void* cmc = LiveMovement(character);
    if (!cmc) return false;
    if (ModeOf(cmc) != P::off::kMOVE_None) return true;
    return SetMode(cmc, P::off::kMOVE_Walking);
}

}  // namespace ue_wrap::character_hold
