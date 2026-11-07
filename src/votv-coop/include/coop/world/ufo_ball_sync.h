// coop/world/ufo_ball_sync.h -- the gamemode's ball UFO (ufo_ballfo_C) on the world-actor lane.
//
// mainGamemode_C::ufo_ball rolls on its own timer and mints the UFO above the local player's camera (EX_CallMath, so
// only the source-gated catch in coop/creatures/npc_world_enum sees it). The host's roll is the only one that runs: a
// client's ufo_ball is refused in coop/world/spawn_authority. The host's ball reaches a client as a world-actor mirror,
// but its root never moves, so the transform alone mirrors nothing: its `set` randomizes the heading and both endpoints,
// and its tick slides the ball component between them. The host's spawn therefore carries those values and the ball's
// current place in the world-actor birth blob, and the mirror keeps its own tick (kWorldActorTickDrivenClasses): the
// class's tick is deterministic given the setup, so the two balls fly the same course. A joiner mid-flight gets the
// current place in the connect snapshot's blob; the host's destruction rides the lane's ordinary destroy.
//
// Birth blob (little-endian floats, 32 bytes): centerYaw, aX, aY, bX, bY, ballsX, ballsY, ballsZ.

#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::ufo_ball_sync {

// Bytes of the birth blob this lane writes.
inline constexpr size_t kBirthBytes = 8 * sizeof(float);

// True if `actor` is a live ufo_ballfo_C. Game thread.
bool IsBallActor(void* actor);

// Host: write the ball's setup into `blob` (capacity `cap`) and its length into `len`. A ball whose components cannot
// be read leaves len 0 and says so, once per ball; the mirror is then born at the class's own random setup. `eid` is
// for the log. Game thread.
void FillBirth(void* actor, unsigned int eid, uint8_t* blob, size_t cap, uint8_t& len);

// Client: apply a birth blob to a freshly finished mirror. A wrong length or a non-finite or out-of-range value
// is dropped, named, and leaves the mirror's own setup. Game thread.
void SeedMirror(void* actor, unsigned int eid, const uint8_t* blob, size_t len);

}  // namespace coop::ufo_ball_sync
