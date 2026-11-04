// harness/dedicated_server.h -- the game run headless as a host nobody sits at
// (docs/dedicated-server.md). Boot glue: it replaces the person who would have clicked Host, and
// owns no gameplay. The world is loaded and hosted through the same call the Host button's picker
// makes (coop::session_manager::HostWithSave, drained by the play loop's DriveHostBootIfPending);
// the commands are the chat commands the host operator already has, and the save is the game's own.
//
// Off unless the switch is set (harness/dedicated_server_config.h); every entry below is then a
// no-op, so a player's launch runs exactly as before.

#pragma once

namespace harness::dedicated_server {

// harness::Start, first, on the boot thread, before any config row is read and before the timeline
// thread exists. When the switch is set: the settings are logged; the net rows the server configures
// are written to their env twins (net.port, net.nick) and the scenario is pinned to `menu`; the
// stdout mirror becomes the log's sink; the console control handler is installed. Returns Enabled().
bool ApplyBootOverrides();

// Whether the overlay, the in-game console's sink and the other surfaces that need a person or a
// renderer are skipped this process. Any thread.
bool SkipInteractiveSurfaces();

// The menu scenario, on the timeline thread, just before RunPlayLoop: the driver (resolve the save,
// queue the host, wait for the session, print the ready line, run the autosave and watch the
// session) and the two command readers (stdin, and server_commands.txt in the mod's data folder).
// Returns at once.
void Launch();

}  // namespace harness::dedicated_server
