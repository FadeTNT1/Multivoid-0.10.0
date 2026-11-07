// ue_wrap/engine/travel_residue.cpp -- see ue_wrap/engine/travel_residue.h.

#include "ue_wrap/engine/travel_residue.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>

namespace ue_wrap::travel_residue {
namespace {

namespace P = ue_wrap::profile;
namespace N = ue_wrap::profile::name;
namespace R = ue_wrap::reflection;

constexpr uint8_t kMoveNone = 0;
constexpr uint8_t kMoveWalking = 1;

// A property's offset on the live game instance's class, or -1. Looked up per call: these run once
// per refused trip, and a class held across a world change is not to be trusted.
int32_t GameInstanceOffset(void*& gi, const wchar_t* prop) {
    gi = ue_wrap::world_singleton::GameInstance();
    if (!gi || !R::IsLive(gi)) return -1;
    return R::FindPropertyOffset(R::ClassOf(gi), prop);
}

}  // namespace

std::wstring ReadLoadedSlotName(bool& ok) {
    ok = false;
    void* gi = nullptr;
    const int32_t off = GameInstanceOffset(gi, N::GameInstanceLoadedSlotNameProp);
    if (off < 0) return {};
    ok = true;
    return ue_wrap::field_io::ReadFStringAt(gi, off);
}

bool WriteLoadedSlotName(const std::wstring& name) {
    void* gi = nullptr;
    const int32_t off = GameInstanceOffset(gi, N::GameInstanceLoadedSlotNameProp);
    if (off < 0) return false;
    return ue_wrap::field_io::WriteFStringField(gi, off, name);
}

bool RestoreCurrentLevelIfNone(const wchar_t* level, bool& wasNone) {
    wasNone = false;
    if (!level || !*level) return false;
    void* gi = nullptr;
    const int32_t off = GameInstanceOffset(gi, N::GameInstanceCurrentLevelProp);
    if (off < 0) return false;
    auto* slot = reinterpret_cast<R::FName*>(reinterpret_cast<uint8_t*>(gi) + off);
    if (slot->ComparisonIndex != 0 || slot->Number != 0) return true;  // a level is named: nothing to do
    wasNone = true;
    if (!ue_wrap::fname_utils::Resolved()) return false;
    const R::FName lv = ue_wrap::fname_utils::StringToFName(level);
    if (lv.ComparisonIndex == 0 && lv.Number == 0) return false;
    *slot = lv;
    return true;
}

bool RestoreWalkingIfStopped(void* character, bool& wasStopped) {
    wasStopped = false;
    if (!character || !R::IsLive(character)) return false;
    void* cmc = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(character) +
                                          P::off::ACharacter_CharacterMovement);
    if (!cmc || !R::IsLive(cmc)) return false;
    const uint8_t mode =
        *(reinterpret_cast<const uint8_t*>(cmc) + P::off::UCharacterMovement_MovementMode);
    if (mode != kMoveNone) return true;
    wasStopped = true;
    void* fn = R::FindDispatchFunction(R::ClassOf(cmc), N::CharacterSetMovementModeFn, nullptr);
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<uint8_t>(N::CharacterSetMovementModeParam, kMoveWalking);
    f.Set<uint8_t>(N::CharacterSetCustomModeParam, uint8_t{0});
    return ue_wrap::Call(cmc, f);
}

}  // namespace ue_wrap::travel_residue
