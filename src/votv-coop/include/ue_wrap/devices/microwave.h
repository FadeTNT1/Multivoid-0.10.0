// ue_wrap/devices/microwave.h -- engine access for the microwave (prop_microwave_C). Principle-7 engine-wrapper layer:
// no network or coop state.
//
// Action 4 cycles `timeMode` mod 5 and sets `timer` to 5, 10, 15, 30 or 60 seconds; action 8 calls `wave()`, which,
// powered (`cordPowered`) with the door shut (`opened` clear), sets `active`, shows `pointLight`, starts the
// `microwave_loop` sound, copies `timer` into `time` and runs a latent loop that spends `time`, turns the `spin`
// plate 60 degrees a second and calls microwave(self) and microwaveElec() on each of `objectsInside`; at 0 it plays
// `microwave_ding`, hides the light and stops the loop. Opening the door (its child prop_microwaveDoor_C, a swinger)
// while active runs forceOpenDoor, the same stop. getData saves none of this. The names are in
// ue_wrap/core/sdk_profile_names.h.

#pragma once

#include <cstdint>

namespace ue_wrap::microwave {

// True iff `obj` is a live prop_microwave_C or a class below it. Game thread.
bool IsMicrowave(void* obj);

struct State {
    bool    active = false;
    bool    opened = false;
    bool    powered = false;
    int32_t timeMode = 0;
    float   timer = 0.f;   // the set time, seconds
    float   time = 0.f;    // what is left of a run, seconds
};

// Read the state. False for anything that is not a microwave or a member that did not resolve.
bool ReadState(void* mw, State& out);

// Write the set mode and time as they are (action 4's result), no verb run.
bool WriteMode(void* mw, int32_t timeMode, float timer);

// Write what is left of a run.
bool WriteTime(void* mw, float time);

// Draw a run as wave() starts it, without its loop: `active`, `time`, the light shown and the loop sound started.
bool ShowRunning(void* mw, float remaining);

// Draw a run's end as the loop or forceOpenDoor ends it: the ding (when `ding`), `time` 0, `active` clear, the light
// hidden and the loop sound stopped.
bool ShowStopped(void* mw, bool ding);

// Turn the `spin` plate by `yawDegrees` about its own up axis, as the loop does each frame.
bool TurnPlate(void* mw, float yawDegrees);

}  // namespace ue_wrap::microwave
