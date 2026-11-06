// coop/items/inventory_death_keep.h -- a void death does not take the player's inventory with it.
//
// A player killed by being thrown out of the world came back with nothing: the revive restores health, stance,
// position and the dead flag, the inventory the death chain emptied was nobody's to restore, and the client then streamed
// the empty inventory to the host (36 KB down to 1 KB), whose autosave persisted it. This module closes the first
// half; coop/player/player_profile_store the second. Per machine, for its own player:
//   - a 1 Hz sample of the inventory while the player is alive and standing (the game's byte codec), kept only while
//     it holds something and, inside a void window, only while it has not collapsed against the sample before;
//   - a void window of a few minutes, opened by the anti-noclip rescue or by a revive whose death place was far under
//     every place the player stood;
//   - inside it, a revive restores the sample once if the inventory collapsed (nothing, or under a quarter of the
//     sample's items) by the game's own restore, which REPLACES what is held, so nothing is added twice, and the
//     profile stream holds a collapsed inventory back until then. A death outside a window is the game's own.

#pragma once

#include <cstdint>

namespace coop::inventory_death_keep {

// Every net-pump tick: sample the local inventory (1 Hz).
void Tick();

// Open the void window now. `why` is logged.
void NoteVoidEvent(const char* why);

// A revive is about to move the local player (the pawn is still where it died): a death place far under every place
// the player stood opens the window. Game thread.
void OnReviveBegin(void* pawn);

// The revive finished (the dead flag is cleared): inside the window, restore the sample once when the inventory
// collapsed. Game thread.
void OnReviveEnd(void* pawn);

// The profile stream's check: true when the inventory the player holds now has collapsed against the sample inside the
// window, so the stream should not send it (the host keeps the last profile it has). Game thread.
bool HoldCollapsedProfile(void* pawn);

// A restore put an inventory on the player (coop/items/inventory_restore): the next tick samples the new one afresh,
// so a window's collapse test is not made against what the player held before.
void RequestResample();

// Session teardown: the sample and the window go.
void OnDisconnect();

}  // namespace coop::inventory_death_keep
