// coop/world/event_fire_policy.h -- the replay policy's one entry, private to event_fire_sync (a
// co-located src header, not include/): the rows themselves are event_fire_policy.cpp.
#pragma once

#include <string>

namespace coop::event_fire_sync {

// 1 replay, 0 known no-replay (laneOut names who carries the outputs, or why nobody does), -1
// unknown -- the default is no replay.
int ReplayVerdict(const std::string& name, const char** laneOut);

// The save key of the level-placed actor a replayed row's trigger reaches by key, or null when the row has none.
// The treehouse build stages (treehouse_0..5) cast the keyed treehouse_C; a peer that has not got it yet shows the
// game's "Invalid actor and actor with the key does not exist" hint, so the replay waits for it.
const wchar_t* RequiredKey(const std::string& name);

}  // namespace coop::event_fire_sync
