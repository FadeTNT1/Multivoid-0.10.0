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
over the game mode, the save's day over `startDay`; `loadObjectsFromSlot` is set and `untitled_1`
opened (`ue_wrap/engine/engine_save`, `LoadWorld`). The rules are also copied over the save's
`localGameRules`, the copy a joiner's transferred save carries and three blueprints read, so a
joiner runs under the host's world rules.

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

The host's `saveToSlot` returning is the moment its world on disk changed: the per-player
profiles are cut beside it then (`coop/items/player_inventory_sync`, keyed on the world folder),
and the dedicated server's autosave runs the same `saveSlot_C::save` (`ue_wrap/engine/world_save`).

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

## Code map

| Concept | Files |
|---|---|
| the world folder and its files | `ue_wrap/engine/world_folder`, `ue_wrap/engine/world_meta` |
| the world list | `ue_wrap/engine/save_browser`, `ue_wrap/engine/gvas_meta` (legacy rows) |
| the load | `ue_wrap/engine/engine_save` (`LoadWorld`), `harness/world_boot` |
| the rules copy | `ue_wrap/world/game_rules` (`CopyRules`) |
| making a world | `ui/host_window_native`, `ui/host_create_world`, `ui/host_save_picker` |
| only the host saves | `coop/save/save_block`, `ue_wrap/engine/save_to_slot_hook`, `ue_wrap/engine/world_save` |
| tests | `harness/autotest/autotest_saveui.cpp` (`VOTVCOOP_RUN_SAVEBLOCK_TEST`, `VOTVCOOP_RUN_SAVEMSG_TEST`) |
