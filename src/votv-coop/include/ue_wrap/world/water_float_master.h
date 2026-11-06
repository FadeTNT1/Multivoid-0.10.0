// ue_wrap/world/water_float_master.h -- engine access for the world's buoyancy master (waterFloatMaster_C), the one
// actor that floats things. Principle-7 engine-wrapper layer: reflection and a list read, no network and no co-op
// state; coop::prop_motion_scan owns those and reaches the engine through here.
//
// Every water volume (the lake, the basement's flood, the aquarium) reports the bodies inside it to this master
// (addObj, on the volume's overlap), and the master's own tick pushes each simulating body it holds toward the surface
// and damps it. `actorsInWater` is its list of those bodies: a body is on it from the moment it touched a volume until
// the volume's end-overlap removes it. The names are in ue_wrap/core/sdk_profile_names.h.

#pragma once

#include <cstddef>
#include <vector>

namespace ue_wrap::water_float_master {

// The actors the buoyancy master currently holds, as pointers: `out` is cleared and filled, null entries dropped. The
// pointers are not checked for life here (an entry whose actor died is the master's own housekeeping to clear); a caller
// that acts on one checks it with reflection::IsLive first. Returns the count. 0 when the master is not in this world,
// the list does not resolve (said once) or it is empty. Game thread.
size_t ActorsInWater(std::vector<void*>& out);

}  // namespace ue_wrap::water_float_master
