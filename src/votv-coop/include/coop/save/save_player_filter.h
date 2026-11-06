// coop/save/save_player_filter.h -- only the local player's own record reaches the world save.
//
// The game's save walk (mainGamemode_C::saveObjects) gathers every actor implementing int_save and
// writes one record per actor, each keyed by the actor's `Key`. mainPlayer_C implements it, answers
// ignoreSave false, and keys itself "mainPlayer" -- and the mod spawns more mainPlayer_C actors than
// the local player: every peer's puppet, and the skins panel's preview mannequin. Each of them was
// written into the host's save as an empty "mainPlayer" record beside the host's own; on the next
// load the loader handed every record of a key to the one live player, so the last, empty record
// emptied the host's inventory, hotbar and equipment (docs/saving.md, "One player record").
//
// This module answers mainPlayer_C::ignoreSave true for every mainPlayer_C that is not the local
// player, at the script gate, so the walk skips it. It also logs, on the host, what the player
// holds as its record is captured (betterGetData), so a save that captured an empty inventory is
// visible in the log. The load side (a duplicate record refused) is ue_wrap/engine/save_restore_watch.

#pragma once

namespace coop::save_player_filter {

// Register the watches. Idempotent, any role. Called at session start and by the skins preview;
// game thread.
void Install();

// Drive the watches' name resolve until they have settled. Cheap once settled. Game thread.
void Pump();

// The preview mannequin exists outside a session, where nothing else holds the script gate: it
// holds the gate for as long as the mannequin stands. Idempotent per state; game thread.
void HoldForPreview(bool on);

// Counters for logs and self-tests.
struct Stats {
    unsigned excluded = 0;  // ignoreSave answers forced true
    unsigned captured = 0;  // betterGetData captures of the local player seen
};
Stats Counters();

}  // namespace coop::save_player_filter
