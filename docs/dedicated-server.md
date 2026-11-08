# Dedicated server

A friend group wants one world that stays up while nobody sits at the host: a box in a closet, a
VPS, a Pterodactyl panel. Voices of the Void ships one executable, a Win64 *shipping client*;
there is no UE dedicated-server target and none can be built without the game's source. So the
dedicated server is **the game itself, run headless, with Multivoid hosting from it**. The host
remains a listen server in the engine's sense: one local player, one world, the mod's session on
top. Everything the mod already does as a host is unchanged; server mode only replaces the
person who would have clicked through the menu.

This page is the design, the switch and its configuration, and how to run it on Windows, under
Wine, and in a Pterodactyl egg (`server-docker/`). What is stubbed and what needs a live test is
at the end.

## The shape

```
VotV-Win64-Shipping.exe -nullrhi -nosound -unattended -nosplash -NoVerifyGC -multivoid-server
   UE4SS loads main.dll  ->  harness::Start
     dedicated_server::ApplyBootOverrides   server.ini + env + command line -> the net rows' env twins,
                                            the stdout mirror, the console control handler
   TimelineThread (scenario "menu")
     dedicated_server::Launch               the driver thread, the command reader
       HostWithSave(slot, Direct|Brokered)  the SAME call the Host button's picker makes
     RunPlayLoop                            drains it: DriveHostBootIfPending loads the save, starts the host
       -> "rig: READY hosting"  and  "[server] Multivoid dedicated server ready"
   stdin / server_commands.txt  ->  stop | save | say | players | kick | <any chat command>
```

### What headless means for the engine

| Flag | Effect | Why |
|---|---|---|
| `-nullrhi` | the NULL RHI: no GPU device, no swapchain, no present | no GPU in a container; the world, physics, AI, Blueprints and timers still tick |
| `-nosound` | no audio device | containers have none; the mod's Opus voice is untouched (it never opens a device on a peer that does not talk) |
| `-unattended` | no engine dialogs (crash reporter, ensure popups) | nothing can click them |
| `-nosplash` | no startup splash window | |
| `-NoVerifyGC` | skip the GC verification pass | shorter hitches; shipping builds mostly skip it anyway |
| `-RenderOffScreen` | a real RHI that renders without a window | the fallback when a NULL RHI breaks something the game needs (see Xvfb below) |

The engine still runs its game loop at full speed with `-nullrhi`, and the shipping build has
no frame cap of its own, so a NULL-RHI server spins a core. The mod issues `t.MaxFPS <max_fps>`
(default 30) once the world is up. The simulation stays frame-rate independent where the game
already is; a game system that counts frames would run at that rate, which is the same as a slow
player's machine.

### What the mod depends on that breaks headless

| Dependency | Headless | Server mode does |
|---|---|---|
| the ImGui overlay (DXGI/D3D12/Vulkan present hooks) | no present ever fires under `-nullrhi` | `imgui_overlay::Init` is skipped. The overlay already fails closed (no present, no draw), but skipping it also avoids creating a dummy D3D device to find the vtable. Every F1 panel, the ImGui fallback browser and the ImGui dialogs (`game_build_dialog`, `boot_warning_dialog`) are therefore absent; their facts are in the log |
| the in-game console's log sink | nothing draws it | not installed; the log sink is the stdout mirror instead (the logger has one sink) |
| the game-thread pump | rides `ProcessEvent`, not the present | unchanged: the engine dispatches `ProcessEvent` every tick with or without an RHI |
| the play loop and session tick | the TimelineThread, `harness::pump` | unchanged |
| the main-menu flow (content warning, menu widgets, Multiplayer button injection) | UMG widgets exist under the NULL RHI; Slate just never paints | not driven. `LoadStorySave` re-issues `open` until the engine takes it, which is what the `[dev] menu_autoload` lane and the test `VOTVCOOP_TEST_HOST_SAVE` lane already rely on, and it is not blocked by the content-warning screen |
| the host's save picker and the native hosting window | need a person | bypassed: server mode calls `session_manager::HostWithSave` itself with the configured slot, name, password, player cap and reachability, the exact call `ui/host_save_picker.cpp` makes; no join cover is raised (nobody is looking) |
| input (`input_owner`, the game window subclass) | a window may not exist (`-nullrhi` still creates the viewport window on Windows; under Wine without a display it may not) | unchanged; every consumer already tolerates no window. The shutdown path that rides `WM_CLOSE` is replaced by the console stop |
| `MessageBoxW` (the refuse dialog) | blocks its own thread only | unchanged: it is already on a thread of its own, and the reason is logged first |
| F12 screenshots | nothing to capture | harmless no-op |

### The server's own pawn

The host is a player in this design: the engine has one local `mainPlayer_C` and the session's
slot 0 is the host's. There is no engine-level spectator to fall back on (the shipping game has
no spectator pawn class worth driving, and taking the host's pawn away would break every lane
that reads "the local player" -- the save, the time, the gamemode's per-player Blueprint state).
The decision for the first cut: **the pawn is parked where the save left it**, named by its
nickname (`host_nick`, default `Server`) so the scoreboard is honest about the fourth body. It is
visible and collidable. It uses one of the ten peer slots (`kMaxPeers` is a wire constant; nine
friends plus the server, fewer when `max_players` is lowered). The death guard already answers a death with a revive rather than a trip
to the menu (`coop/player/death_revive`), so a creature that kills the parked body does not end
the world.

Hiding the pawn (actor hidden, collision off, teleported under the map or into a sealed room) is
deliberately NOT done yet: it changes what every lane that reads the local player sees, and it
needs a live test per lane. It is the first follow-up (see *Stubbed*).

### Auto-loading and auto-hosting

`save_slot` names the world. The driver hosts the world folder `SaveGames/<slot>/` when it holds
a `main.meta` (the game's 0.10.0 world layout, [ui.md](ui.md)); else a **new story world named
`<slot>`** is created through the game's own `createSaveSlotTreeDirectory`, the primitive the
picker's New game uses, so the next boot finds that folder.

Then `HostWithSave` is queued with:

- **reach** `direct` (default): our own UDP listen on `port`, on every interface. The container
  maps the port. `list_public=1` announces it to the master with that port, and the master hands
  joiners `ip:port`; `list_public=0` makes zero master calls (`IsMasterFree`), and friends use
  Direct Connect.
- **reach** `brokered`: the master's signaling and TURN, always listed. For a box behind NAT with
  no port forward.

The play loop's existing `DriveHostBootIfPending` performs the load and the session start. The
driver waits for `session.running()` with the Host role, then prints the ready line the panel
watches for. A host that never comes up within `ready_timeout_s` (default 300) exits the process
with code 3, so a panel's crash restart tries again rather than leaving a zombie at the menu.

### Configuration

Precedence: built-in default < `Mods\Multivoid\server.ini` `[server]` < environment < command
line. The server's values are written into the existing rows' **env twins** before the first
config read, so no row of `multivoid.ini`, no registry entry and no wire byte changes.

| `server.ini` key | Environment | Command line | Default | Maps to |
|---|---|---|---|---|
| `port` | `MULTIVOID_PORT` | `-mvport=` | `47621`* | `VOTVCOOP_NET_PORT` |
| `name` | `MULTIVOID_SERVER_NAME` | `-mvname=` | `Multivoid Server` | lobby name |
| `password` | `MULTIVOID_PASSWORD` | `-mvpassword=` | empty (open) | `VOTVCOOP_NET_LOBBY_PASSWORD` + lock |
| `max_players` | `MULTIVOID_MAX_PLAYERS` | `-mvmaxplayers=` | `10` (clamped 2..10, server included) | lobby cap; seats past it are refused (`MV-H21`, the server is full) |
| `save_slot` | `MULTIVOID_SAVE_SLOT` | `-mvslot=` | `multivoid_server` | the world |
| `list_public` | `MULTIVOID_LIST_PUBLIC` | `-mvlist=` | `1` | `HostMode.listed` |
| `reach` | `MULTIVOID_REACH` | `-mvreach=` | `direct` | `HostMode.reach` |
| `host_nick` | `MULTIVOID_HOST_NICK` | `-mvnick=` | `Server` | `VOTVCOOP_NET_NICK` |
| `autosave_minutes` | `MULTIVOID_AUTOSAVE_MINUTES` | `-mvautosave=` | `10` (0 = off) | the autosave timer |
| `max_fps` | `MULTIVOID_MAX_FPS` | `-mvmaxfps=` | `30` (0 = leave) | `t.MaxFPS` |
| `stdout` | `MULTIVOID_STDOUT` | `-mvstdout=` | `1` | the stdout mirror |
| `ready_timeout_s` | `MULTIVOID_READY_TIMEOUT` | | `300` | the boot bound |

\* when the key is absent, `net.port` keeps its own value (`coop::net::kDefaultPort`, 47621); the
egg sets `SERVER_PORT` explicitly.

The switch itself: `-multivoid-server` on the command line, or `MULTIVOID_SERVER=1`. Without it
nothing on this page runs and a player's launch is byte-for-byte the same as before.

### Autosave

Every `autosave_minutes`, while the host session runs, the driver posts the game's own full save
(`saveSlot_C::save`, the gather and the write the game's pause-menu save and autosave run) to
the game thread; it writes into the hosted world's folder. The mod's save hooks ride that call: the player profiles are cut to disk with
the world (`coop/player/player_profile_store`), and the pre-session backup already ran at the
host start (`coop/save/save_guard`). A non-forced save during a running event writes the world of
the last gather (see `ue_wrap/engine/save_capture.h`), which is what the game itself does; the
next autosave catches up. `save` on the console runs one now; `stop` runs one before it exits.

### The console

Lines arrive from two sources, both always read:

- **stdin**, when the process has one (a pipe, a file, a console). Pterodactyl writes the panel's
  console input there.
- **`Mods\Multivoid\server_commands.txt`**, polled once a second, each appended line run once and
  the file emptied. Wine does not always hand a GUI-subsystem exe a usable stdin, and a file is
  the one transport every launcher can write; the Docker entrypoint uses it.

| Command | Does |
|---|---|
| `stop` (`quit`, `exit`) | save, withdraw the lobby, stop the session, ask the engine to quit; a watchdog ends the process after 30 s |
| `save` | one save now |
| `say <msg>` | a chat line from the host, to every player |
| `players` (`list`) | the roster: slot, session id, nickname, ping, player id |
| `kick <name>` | the chat command `/kick <name>` as the host operator |
| `help` | this list |
| anything else | run as a chat command (`/ban`, `/tphere`, `/set`, `/mv`, `/time`...) as the host operator; the reply is logged |

Output: every log line is mirrored to stdout (`[INFO] ...`) when `stdout=1` and the process has an
stdout handle (it tries `AttachConsole(ATTACH_PARENT_PROCESS)` when it has none). `multivoid.log`
beside the exe is written as always, so `tail -F` of it is the fallback the entrypoint uses.

Ready line, for a panel's done-detection: `[server] Multivoid dedicated server ready` (and the rig's
existing `rig: READY hosting`, which comes first).

### Graceful shutdown

The stop sequence, from any source (the `stop` line, `CTRL_C/CTRL_BREAK/CTRL_CLOSE/CTRL_SHUTDOWN`,
the entrypoint's `SIGTERM` trap writing `stop` to the command file):

1. one save, waited for on the game thread (bounded at 60 s);
2. `EndHostedLobby` (the master is told; a no-op when unlisted);
3. `quit` posted to the game thread through `ExecuteConsoleCommand`, which asks the engine loop to
   exit at the end of its frame;
4. `coop::shutdown::DoShutdown` (the session stop, the detours lifted), the same sequence the
   window close runs, so the engine's teardown never re-enters our hooks;
5. a 30 s watchdog that flushes the log and calls `ExitProcess` if the engine has not exited.

A host session that stops by itself after the ready line (a net fault) logs why and runs the
same sequence with exit code 2, so the panel restarts the server.

## Running it

### Windows

```
VotV-Win64-Shipping.exe -nullrhi -nosound -unattended -nosplash -NoVerifyGC -multivoid-server -mvslot=myworld -mvport=47621
```

`server-docker/run-dedicated.ps1` does the same with parameters, from a game folder that already
has UE4SS and Multivoid installed.

### Linux: Wine

Pterodactyl nodes run Linux containers, so the game runs under Wine (wine-staging, 64-bit
prefix). `-nullrhi` means no GPU, no DXVK and no Vulkan driver are needed. UE4SS loads through
its `dwmapi.dll`/`xinput1_3.dll` proxy; under Wine the proxy needs a native override:
`WINEDLLOVERRIDES="dwmapi=n,b"` (or `xinput1_3=n,b`, whichever proxy the install uses).

The save lives in the Wine prefix's `%LOCALAPPDATA%\VotV\Saved\SaveGames`, i.e.
`$WINEPREFIX/drive_c/users/<user>/AppData/Local/VotV/Saved/SaveGames`; the container keeps the
prefix inside the persistent volume so the world survives a rebuild.

**A display.** `-nullrhi` needs no GPU, but the engine still creates its window through
`user32`, and Wine's X11 driver wants a display for that. The start script therefore runs a bare
`Xvfb` by default (`USE_XVFB=1`): a display server only, nothing renders to it. `USE_XVFB=0` tries
without one (Wine's null graphics driver); whether the game survives that is a live-test question.

**Software-RHI fallback.** If the NULL RHI breaks something the game needs (a Blueprint that reads
the viewport size, a render target the save path uses), run a real RHI on the virtual display
instead: `RHI=software` drops `-nullrhi`, adds `-RenderOffScreen` and sets
`LIBGL_ALWAYS_SOFTWARE=1`, so Wine's D3D11 runs on Mesa's llvmpipe. It costs CPU but needs no GPU.

### Pterodactyl

`server-docker/egg-multivoid.json` is the egg; `server-docker/Dockerfile` the image. Game files are
**not redistributable**: the egg's install script creates the folder layout and stops; the owner
uploads their own `VotV` game folder (with UE4SS and Multivoid installed, exactly as on their own PC)
into the server's files through the panel's file manager or SFTP. The egg's variables:

| Variable | Default | |
|---|---|---|
| `SERVER_PORT` | the allocation's port | UDP |
| `SERVER_NAME` | `Multivoid Server` | |
| `PASSWORD` | empty | empty = open |
| `MAX_PLAYERS` | `10` | 2..10; the server is one of them |
| `SAVE_SLOT` | `multivoid_server` | |
| `LIST_PUBLIC` | `1` | |
| `AUTOSAVE_MINUTES` | `10` | |
| `REACH` | `direct` | `brokered` for a node without an open port |
| `USE_XVFB` | `1` | the virtual display above |
| `RHI` | `null` | `software` is the fallback above |

Stop command `stop`; done-detection `Multivoid dedicated server ready`.

## Stubbed, and what needs a live test

Working by construction (the code paths are the ones the Host button and the test lanes already
run): the switch, the config layering, the save resolution, `HostWithSave`, the ready line, the
console commands, the autosave call, the stop sequence, the stdout mirror.

Not done:

- **the pawn is not hidden** (above);
- **no restart-in-place** after a session fault: the process exits and the panel restarts it;
- **no idle pause**: the world runs with nobody connected, as a host left at its desk does
  (hunger, the clock, events). A `pause_when_empty` is a follow-up and needs the game's own pause
  verb, which `pause_guard` currently fights.

Needs a live run, in this order of risk:

1. the game reaching gameplay at all under `-nullrhi` (a Blueprint that needs a viewport, the
   content-warning widget, a render target in the save path);
2. the same under Wine (UE4SS's proxy load, `GetCommandLineW` carrying the flags, the stdin
   handle a GUI exe gets);
3. `saveToSlot` from a worker-posted task writing the canonical slot on the host;
4. the `quit` console command ending a shipping build's loop, and the teardown after
   `DoShutdown`;
5. the parked pawn's sleep: if the game's night skip waits for every player to rest, the server's
   pawn never rests (`coop/player/sleep_sync`).

Test steps are in [server-docker/README.md](../server-docker/README.md).
