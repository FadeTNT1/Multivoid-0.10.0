// coop/save/save_block.h -- only the host saves. The host's world is the one canonical save,
// written into its own world folder; a client never writes a world file. Every world-save verb is
// gated at its body through the script gate: the gamemode's save (the pause menu's and the
// quicksave key's entry), saveSlot_C's save, saveToSlot, saveManual, savePlayerOnly and saveSelf,
// and the metadata's and the persistent object's writes. The role is read when the call fires: a
// client of a running session, or a process whose loaded world is still that client's mirror, is
// refused; a host and a solo world run. A save the client asked for (the gamemode's save from the
// player or the pause menu, not an autosave) puts "You're not the host, you can't save." in its
// chat. Two belts: gamemode.disableSave held true on a client's mirror, and the SaveGameToSlot
// detour (ue_wrap/engine/save_to_slot_hook) refusing a flat-slot world write. docs/saving.md.

#pragma once

namespace coop::net { class Session; }

namespace coop::save_block {

// Register the gates. Idempotent, in every role. Called at session start, before a joiner's world
// loads (a client's post-load save must meet the gate), and again by the world-up install pump.
void Install(coop::net::Session* session);

// Drive the gate's name resolve until every save-verb watch has settled. Every session tick from
// the first, the menu-mode join's transfer wait included. Game thread.
void Pump();

// Hold gamemode.disableSave=true on the client of a running session (a no-op on the host). Cheap
// steady state. Call per gameplay pump tick.
void Tick(coop::net::Session* session);

// Called on the game thread after saveSlot_C::saveToSlot ran on a peer that may save, with the save
// object that was written: the moment the world on disk changed. One listener.
using WrittenFn = void (*)(void* saveObject);
void SetWritten(WrittenFn fn);

}  // namespace coop::save_block
