// ue_wrap/engine/physical_audio.h -- a physical material's sound cues (0.10.0 per-material audio).
//
// The game maps a UPhysicalMaterial to a physMat_SoundsAndParticles_C object through
// lib_C::getPhysicalAsAudio (a material that is not one resolves to the physsnd_fallback asset).
// That object carries the cues: `sound_soft` is what a native pickup plays, `sound_impact` what an
// impact plays. The old lib_C::physSound is obsolete in 0.10.0 -- calling it raises the in-game hint
// "obsolete function: phys" -- so nothing here or anywhere may call it.
//
// Every lookup resolves by reflected name (names in sdk_profile_names.h) and fails safe: a miss
// returns null, is logged once, and never reaches the player as a hint. Game thread only.

#pragma once

namespace ue_wrap::physical_audio {

enum class Cue {
    kSoft,       // sound_soft: the grab / pickup cue
    kImpact,     // sound_impact: the land / impact cue
    kDamaged,    // sound_damaged: a hit that took damage
    kDestroyed,  // sound_destroyed: a break
};

// The cue (a USoundBase*) of a UPhysicalMaterial, through lib_C::getPhysicalAsAudio. `worldCtx` is
// any live object in the world. Null when the material is null or dead, the library or function is
// unresolved, the call fails, or the audio material has no such cue.
void* CueFromPhysicalMaterial(void* physMat, void* worldCtx, Cue which);

// The cue of the material a prop_C's physicsImpact component holds (physmat_override when valid,
// else physmat). Null for an actor with no such component, or on any miss as above.
void* CueFromPropImpactComponent(void* propActor, Cue which);

}  // namespace ue_wrap::physical_audio
