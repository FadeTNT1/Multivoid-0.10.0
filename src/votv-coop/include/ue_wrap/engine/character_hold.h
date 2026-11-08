// ue_wrap/engine/character_hold.h -- a character held where it stands by its movement component's
// own mode: MOVE_None stops locomotion and jump physics wholesale while the controller's look stays
// live. The same mode the freecam and the wisp's grab put the local player in, through the engine's
// own SetMovementMode, so the component's transition side effects run.
//
// Engine-wrapper layer (principle 7): no gameplay or co-op state. Game thread only. The names are in
// ue_wrap/core/sdk_profile_names.h (CharacterSetMovementModeFn and its parameters).

#pragma once

namespace ue_wrap::character_hold {

// What a hold call did.
enum class Hold : unsigned char {
    Unresolved,  // the character, its movement component or the verb does not resolve
    Airborne,    // the character is falling: held later, so it does not hang in the air
    Holding,     // the character's mode is MOVE_None now (this call set it, or it already was)
};

// Put `character` in MOVE_None unless it is falling. Cheap when it already holds: one byte read.
Hold HoldStill(void* character);

// When `character` is held (MOVE_None), give it MOVE_Walking back. True when it is not held after the
// call (it was released, or it was not held); false when the component or the verb does not resolve.
bool Release(void* character);

}  // namespace ue_wrap::character_hold
