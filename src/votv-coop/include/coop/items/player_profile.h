// coop/items/player_profile.h -- everything about one player that is theirs and not the world's:
// what they carry, wear and hold, their vitals, and where they stood.
//
// The game keeps all of it on the one save object a world is built from, and a joiner's world is
// built from a capture of the HOST's. So each part is replaced on the joiner's side as its world
// loads, or the joiner starts as a copy of the host: the host's items, the host's hunger, the
// host's position. coop/items/inventory_wire carries this value, coop/items/player_inventory_sync
// stores it per player on the host and hands it back at a join.

#pragma once

#include "ue_wrap/actors/vitals.h"

#include <cstdint>
#include <vector>

namespace coop::player_profile {

// The last place the player was STANDING: on the ground, not ragdolled, not seated on anything
// (the ATV seats its driver through the same field as a chair). A position read while driving or
// falling is somewhere a body cannot be put back. Not valid on a profile that was never streamed
// by its player (a first join), which places the player at the start point instead.
struct Pose {
    float x = 0, y = 0, z = 0, yaw = 0;
    bool  valid = false;
};

struct Profile {
    // The player's inventory component as the game's own save serializes it
    // (ue_wrap/actors/inventory_component): carried, held and worn, item state included. Without
    // one (a first join) the player takes the game's own starting kit.
    bool                                hasItems = false;
    std::vector<uint8_t>                items;
    // The vitals as read, or none: a save object whose vitals do not resolve must not stop the
    // items from being stored. A profile without them is applied with the game's defaults.
    bool                                hasVitals = false;
    ue_wrap::vitals::Snapshot           vitals;
    Pose                                pose;
};

}  // namespace coop::player_profile
