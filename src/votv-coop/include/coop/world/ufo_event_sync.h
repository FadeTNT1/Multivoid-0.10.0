// coop/world/ufo_event_sync.h -- the gamemode's UFO rolls on the world-actor lane: the ball (ufo_ballfo_C), the boofo
// fly-by (ufo_boofo_spawn1_C), the radio tower's UFO (ufo_joel_C) and the lake glow's UFO (ufo_pillfo_C).
//
// mainGamemode_C rolls each of them on its own timer (ufo_ball, ufo_boo, ufo_joel, ufo_pill) and mints the actor under
// EX_CallMath, so only the source-gated catch in coop/creatures/npc_world_enum sees it, and the eventer's lane never
// does. The host's roll is the only one that runs: a client's is refused in coop/world/spawn_authority. The host's actor
// reaches a client as a world-actor mirror whose actor tick or timeline is its own, so what has to cross is what makes
// the two flights differ: the ball's heading and endpoints (coop/world/ufo_ball_sync), the boofo's random course (the
// angle of each point of its spline) and its progress, and a timeline-driven UFO's place in its timeline, for a joiner
// mid-flight. The births ride the world-actor spawn's blob, the connect snapshot re-sends it with the current progress,
// and the host's destruction rides the lane's ordinary destroy. ufo_midas, which activates a level-placed UFO instead of
// minting one, is an event scene (coop/world/event_scene_sync).
//
// Birth blobs (little-endian): ball -- ufo_ball_sync.h; boofo -- f32 alpha, u8 count, count x u8 angle (256ths of a
// turn); joel and pillfo -- u8 phase (0xFF: none running), f32 seconds into that timeline.

#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::ufo_event_sync {

// True if `actor` is a live actor of one of these classes. Game thread.
bool IsBirthActor(void* actor);

// Host: write the actor's birth blob into `blob` (capacity `cap`) and its length into `len`. An actor whose setup cannot
// be read leaves len 0 and says so once; its mirror is then born at the class's own setup. `eid` is for the log. Game
// thread.
void FillBirth(void* actor, unsigned int eid, uint8_t* blob, size_t cap, uint8_t& len);

// Client: apply a birth blob to a freshly finished mirror. A wrong length or an unusable value is dropped, named, and
// leaves the mirror's own setup. Game thread.
void SeedMirror(void* actor, unsigned int eid, const uint8_t* blob, size_t len);

// Client, each tick: the seeks that wait for a mirror's timeline to start (a joel's starts a second after its begin-play).
void TickClient();

// Teardown: the waiting seeks dropped.
void OnDisconnect();

}  // namespace coop::ufo_event_sync
