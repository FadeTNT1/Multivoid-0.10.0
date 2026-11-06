// ue_wrap/world/basement_flood.h -- engine access for the basement's water volume (waterVolume_basementFlooder_C),
// the one actor the flood is. Principle-7 engine-wrapper layer: reflection, calls and offsets, no network and no
// co-op state; coop::basement_flood_sync owns those and reaches the engine through here.
//
// The level is the actor's distance from its dry position `loc_a`, in world units (getLevel); the actor is moved to a
// level by setLevel and the water's own data (surface plane, depth, buoyancy boxes, the depths the player and the
// props track) is refreshed by the parent's updateWaterData. The names are in ue_wrap/core/sdk_profile_names.h;
// everything resolves by name on the live class, never by number.

#pragma once

namespace ue_wrap::basement_flood {

// The live basement water volume of the running world, or null (no such level, not streamed in yet).
// One-of-a-kind: the object index answers, no walk of the object array. Game thread.
void* Actor();

struct State {
    float level = 0.f;    // distance from the dry position, world units
    float height = 0.f;   // the full level (`height`), the most setLevel can meaningfully take
    bool  moveTo = false; // the cheat's toggle, as startMoving last set it
    bool  begun = false;  // the actor has begun play (its dry position is set); a state read before that is not a level
};

// Read the actor's state through its own getLevel and its fields. False when the class's verbs or fields do not
// resolve (said once), `flood` is not a basement water volume or it has not begun play.
bool Read(void* flood, State& out);

// Place the actor at `level` (setLevel) and refresh the water's data (updateWaterData). False when either verb does
// not resolve or the call fails.
bool ApplyLevel(void* flood, float level);

// The basement drain's `blockage` (0..1): how much of its sample points a prop clogs, which sets how fast it drains. The
// drain recomputes it each second from the props around it; a write is the host's word until the next recompute.
// False when there is no drain in this world or the field does not resolve.
bool ReadBlockage(float& out);
bool WriteBlockage(float value);

// The cheat toggle's value as the host has it: startMoving(moveTo), whose body also enables the actor's tick.
bool ApplyMoveTo(void* flood, bool moveTo);

}  // namespace ue_wrap::basement_flood
