# Saving

## Purpose

Where a co-op world lives on disk and who may write it. The host's world is the one canonical
save, and it is the host's own: a world the host made in the game, or in the hosting window, saved
into that world's folder by the game's own save. A client never writes a world file. This page
covers the 0.10.0 world layout, how the mod lists, creates and loads a world, and the gate that
keeps a client's disk untouched.

## How it works

### A world is a folder

From 0.10.0 the game keeps each world in `SaveGames/<world>/`: `main.sav`, `main.meta` and
`main.png` for the world, the same three for its `autosave`, and `persistent.sav`, with `areas/`,
`local/` and `backups/` beside them. Every file is written by the game's CompressedSaveLoadLib
plugin: UE's chunked compression (tag `0x9E2A83C1`, zlib) over an int32 length, the SaveGame's class
path and a tagged property stream, with no GVAS header. `[V]` read off three worlds made in vanilla
0.10.0. The `.meta` holds a `save_metadata_C`: the version, the last-saved date, the game mode, the
world rules (`Rules`, a `struct_gameRules`), the folder and entry names and the level. The
pre-0.10.0 flat slots (`SaveGames/<slot>.sav`, GVAS) are the game's legacy saves; its own Load
menu converts them into folders. `[RD]` `lib_C::convertLegacySaveToCurrent`.

A world is named in the mod by a key: the folder name for its main entry, `<folder>/autosave` for
its autosave (`ue_wrap/engine/world_folder`).

### The list

The host's world list reads each folder's `main.meta`, and `autosave.meta` once the game has
written an autosave (a fresh world's autosave is a blank placeholder), inflating them with the
vendored miniz on a worker and caching each row by modification time
(`ue_wrap/engine/world_meta`, `ue_wrap/engine/save_browser`). A field the stream leaves out takes
the class default, the game mode from `save_metadata_C`'s default object. Flat slots are listed
after the worlds as old saves, greyed, with the pointer to the game's Load menu: a world loaded
from one would save to the wrong place. An entry saved inside an area level is listed and not
hostable.

### Loading a world

The host loads a world as the game's save menu does (`[RD]` `ui_saveSlots_C` apply and
`trylaunch`): `LoadGameCompressed` reads the entry's `.sav` and `.meta` and the world's
`persistent.sav`; `mainGameInstance_C::setSaveSlotObject` registers all three with the entry's
name and the folder; the metadata's rules are copied over the game instance's `gameRules`, its mode
over the game mode, the save's day over `startDay`; `loadObjectsFromSlot` is set, the launch level
put in force, and `untitled_1` opened (`ue_wrap/engine/engine_save`, `LoadWorld`). The rules are
also copied over the save's `localGameRules`, the copy a joiner's transferred save carries and
three blueprints read, so a joiner runs under the host's world rules.

The launch level is what the game's own launch leaves before its travel: `trylaunch` sets
`mainGameInstance_C.baseLevel` to the metadata's `starterMap` (None reads as `untitled_1`) and calls
`lib_C::loadLevel(starterMap, ...)`, which sets `currentLevel` to that level on the game instance
and on the metadata, and to None on its way back to the menu. `[RD]` The new level's
`mainGamemode_C::loadObjects` restores the save -- the player's transform, every saved object, the
player's own data through `mainPlayer_C::betterLoadData` (inventory, equipment), the triggers --
only while `currentLevel` names a level; at None it shows the hint "Cannot load objects: current
level none" and restores nothing. `[RD]` The boot travels with a console open, which sets none of
this, so it writes `currentLevel`, `baseLevel` and the metadata's `currentLevel` itself
(`PutLaunchLevelInForce`); `lib_C::loadLevel` is not called, since it saves the metadata to disk
and travels through a gamemode that preLoad does not have. Without it, a world loaded after a
return to the menu (or in a fresh process) kept only its day, which the boot hands over itself,
and opened with the player at the spawn, an empty inventory and none of the world's changes; the
next save then wrote that over the folder. The same holds for the flat-slot load a joiner uses and
for a fresh New Game, so both name the level too. A world whose `starterMap` is another level is
refused by name: the boot and the session know `untitled_1` only.

A load is complete when the save's restore has run in the new level, not when a pawn stands in it
(the spawn point is off the origin): `ue_wrap/engine/save_restore_watch` holds the script gate for
the campaign and watches `loadObjects` (with the `currentLevel` it began under) and the player's
`betterLoadData`. The boot logs the launch level, the restore and the player data; a restore that
ran under None is logged as an error, and one not seen 30 s after the pawn appeared is logged and
let through so the boot never hangs. A gate that cannot see the restore falls back to the pawn.

The save object remembers its own folder (`saveSlot_C.savepath`, stamped at creation), and
`saveSlot_C::saveToSlot` writes the entry, its screenshot, the persistent object and the
metadata there. `[RD]` A flat slot registered without a folder wrote `SaveGames/main.sav`; that is
what the folder load fixes.

### Making a world

The game makes a world with `lib_C::createSaveSlotTreeDirectory(name)`: the folder tree, a main and
an autosave save, their metadata stamped with the version and the game instance's mode, and the
persistent object. Its create pane then stamps the rules from its rules editor, the starter map and
the start day into the new metadata. `[RD]`

The hosting window offers it twice. **New game** creates a story world silently, named "Coop",
"Coop 2", ... with the default rules (`world_folder::CreateUnique`). **Create world** shows the
game's own save menu with its create pane open, by running the handler of the game's own "create"
button, so the player names the world and sets its rules in the game's editor; when the pane
returns to the list, a folder that was not there before is the new world, and the hosting window
takes the switcher back and goes on to step two with it. Leaving the pane without creating returns
to the hosting window and hosts nothing (`ui/host_create_world`).

### Only the host saves

Every world-save verb is gated at its body through the script gate (`coop/save/save_block`):
`mainGamemode_C::save` (the pause menu's Save and the quicksave and backup keys), `saveSlot_C`'s
`save`, `saveToSlot`, `saveManual`, `savePlayerOnly` and `saveSelf`, `save_metadata_C`'s
`saveMetadata` and `saveSelf`, and `save_persistent_C::saveSelf`. The role is read when the call
fires: a client of a running session, or a process whose loaded world is still that client's
mirror, is refused, and the body never runs; the host and a solo world run. A save the client asked
for, the gamemode's save called from the player or the pause menu and not as an autosave, puts one
line in the client's own chat: "You're not the host, you can't save." At most one line in two
seconds, since a held key repeats; automatic saves are refused silently. The game's own refusals
(an event running, the player in mid-air) are inside the body the host runs and stay the game's.

Two belts sit under the gates: `disableSave` held true on a client's gamemode, which
`saveSlot_C::save` returns on before it gathers, and the `SaveGameToSlot` detour
(`ue_wrap/engine/save_to_slot_hook`) refusing a flat-slot write of the world container, which one
trigger blueprint still makes.

The host's `saveToSlot` returning, with its entry file (`<savepath>/<slotName>.sav`) rewritten
since the call began and not empty, is the moment its world on disk changed; the body's own return
value is never assigned and the plugin's result is only printed, so the file is what is read. The
write is logged with its path and size, a call that wrote nothing is logged as an error and counts
for nothing. The per-player profiles are cut beside it then (`coop/items/player_inventory_sync`, keyed on the world folder),
and the dedicated server's autosave runs the same `saveSlot_C::save` (`ue_wrap/engine/world_save`).

### One player record

A world save holds one record per `int_save` actor, and the player's is the record keyed `mainPlayer`
(`mainPlayer_C::betterGetData`: the pawn's data and its `inventoryComponent_player`, which holds the
carried grid, the hands and hotbar and the equipment). `mainPlayer_C::ignoreSave` is a constant
false, and the mod spawns more `mainPlayer_C` actors than the local player: every peer's puppet
(`ue_wrap/actors/puppet_spawn`) and the skins panel's preview mannequin. `[RD]` Each was saved as an
empty `mainPlayer` record beside the host's own (`[V]` the host's 00:05 save held two, a 20261105
one held three; saves of sessions with nobody else in them held one). The loader replays the
records in order, and for a key it finds in the world it calls `betterLoadData` on the live actor
(`mainGamemode_C::loadObjects`, `[RD]` `existingGatherersKeys` / `array_find`), so the second,
empty record emptied the inventory, hotbar and equipment the first had just restored; the next
save wrote that empty player out.

Two guards hold it. `coop/save/save_player_filter` answers `mainPlayer_C::ignoreSave` true for every
`mainPlayer_C` that is not the local player (a registered puppet, or any body beside a known local
player), at the script gate, so the save walk skips it; the preview mannequin holds the gate for as
long as it stands, since no session may be running. And `ue_wrap/engine/save_restore_watch` refuses
a second `betterLoadData` for a player that already took a record in the same `loadObjects`, so a
save written before the fix loads with the first record (the player's own) and not the last. Both
log: the load logs what the player holds after each `betterLoadData`, the save logs what the local
player holds as its record is captured and again when the file is written, and a refused record is a
warning. `tools/savecheck/list_players.py` lists a save copy's `mainPlayer` records and their
inventory counts, read-only.

## Who owns what

| State | Owner | Shape |
|---|---|---|
| the world on disk | the host | its world folder, written by the game's own save |
| the world rules | the world's metadata | copied to the game instance and the save at load |
| which world is hosted | the host's choice in the hosting window | a world key (`coop/save/save_transfer`, `SetHostWorld`) |
| a client's disk | the client, untouched | every world-save verb refused at its body |

## Wire messages

None of its own. The joiner gets the host's world through the save transfer ([join.md](join.md)),
whose stale fallback now streams the hosted entry's compressed `.sav` as it is on disk; the joiner
loads either form.

## Late join

A joiner's world is the host's, captured live at the join. Nothing a joiner does writes a world
file, so a joiner who leaves keeps its own worlds exactly as they were.

## Known limits

| Limit | Evidence |
|---|---|
| Hosting a pre-0.10.0 flat slot is refused; it is converted in the game's own Load menu first | `[RD]` the game's save path writes into a folder the flat load never names |
| Create world and New game make story worlds; another mode's new world is made in the game's own menu and hosted from the list | `[?]` the create pane reads the mode off the game instance |
| A world-save verb another blueprint overrides in a subclass is not gated; none does in 0.10.0 | `[RD]` the capture lists no override |
| After a session ends with the mirror still loaded, the verb gates stop with the session's hold of the script gate; `disableSave` and the flat-slot gate stay | `[RD]` `coop/session/subsystems` holds the gate for the session |
| Folder loading, the client refusal line and Create world are verified by build and by reading the game's bytecode; the in-game run is pending | `[?]` |
| The launch level (`currentLevel`) and the restore watch are verified by build and by reading the game's bytecode; the in-game run of save, menu, reload is pending | `[?]` |
| The save filter and the duplicate-record refusal are verified by build and by the offline save reading above; the in-game run (carried item, hotbar and equipment, save, menu, re-host, with a client and the skin preview) is pending | `[?]` |
| A world whose metadata names a starter map other than `untitled_1` is refused | `[RD]` every boot and session path keys on `untitled_1` |

## Code map

| Concept | Files |
|---|---|
| the world folder and its files | `ue_wrap/engine/world_folder`, `ue_wrap/engine/world_meta` |
| the world list | `ue_wrap/engine/save_browser`, `ue_wrap/engine/gvas_meta` (legacy rows) |
| the load | `ue_wrap/engine/engine_save` (`LoadWorld`, `PutLaunchLevelInForce`), `ue_wrap/engine/save_restore_watch`, `harness/world_boot` |
| the rules copy | `ue_wrap/world/game_rules` (`CopyRules`) |
| making a world | `ui/host_window_native`, `ui/host_create_world`, `ui/host_save_picker` |
| only the host saves | `coop/save/save_block`, `ue_wrap/engine/save_to_slot_hook`, `ue_wrap/engine/world_save` |
| one player record | `coop/save/save_player_filter`, `ue_wrap/engine/save_restore_watch`, `tools/savecheck/list_players.py` |
| tests | `harness/autotest/autotest_saveui.cpp` (`VOTVCOOP_RUN_SAVEBLOCK_TEST`, `VOTVCOOP_RUN_SAVEMSG_TEST`) |
