// ue_wrap/devices/water_shooter.h -- engine access for the portable pressure washer
// (prop_waterShooter_C) and its water shots (prop_sponge_p_C). Principle-7 wrapper: class tests, raw
// field reads and the washer's three effect assets; no network or coop state (coop::washer_fx owns
// those).
//
// The washer's firing is its own tick: with `fire` set and `level` above zero, each frame fires one
// shot from the holder's camera and plays wade_Cue in 2D. A shot spawns eff_waterThrower where it is
// born; where it lands it plays wade_Cue, spawns eff_waterThrowSplash and cleans. Running that tick on
// a peer that does not own the washer would fire real shots there and clean a second time, so a
// remote washer is shown with these assets only.

#pragma once

namespace ue_wrap::water_shooter {

// Class-pure: `actor` is a prop_waterShooter_C (or derives from it). False while the class is not
// loaded. Game thread.
bool IsWaterShooter(void* actor);

// Class-pure: `obj` is a washer shot, prop_sponge_p_C. Game thread.
bool IsShot(void* obj);

// The washer's trigger and tank: `fire` (the use button is held) and `level` (litres left). Raw field
// reads. False when `actor` is not a live washer or a field does not resolve. Game thread.
bool ReadFiring(void* actor, bool& fire, float& level);

// The three assets a washer's spray is made of, loaded with the washer's class. All non-null once
// resolved. Null until each is found; a miss is retried at most every few seconds (the lookup walks
// the object array). Game thread.
struct FxAssets {
    void* spray;   // eff_waterThrower, ParticleSystem: the jet at the nozzle
    void* splash;  // eff_waterThrowSplash, ParticleSystem: where a shot lands
    void* cue;     // wade_Cue, SoundCue: the firing hiss and a shot's landing
};
const FxAssets* ResolveFx();

}  // namespace ue_wrap::water_shooter
