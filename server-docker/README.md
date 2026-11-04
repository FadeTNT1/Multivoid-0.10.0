# Multivoid dedicated server: Docker, Pterodactyl, Windows

The game itself, run headless with Multivoid hosting from it. The design, the settings and the
console commands are in [docs/dedicated-server.md](../docs/dedicated-server.md); this folder is how
to run it.

**The game is not included and cannot be.** Voices of the Void is not redistributable. Every route
below expects *your own* game folder, with UE4SS and Multivoid installed exactly as on your PC.
Check that hosting from the Multiplayer menu works on your PC first.

| File | What it is |
|---|---|
| `Dockerfile` | Debian bookworm, WineHQ wine-staging, Xvfb, Mesa, tini |
| `entrypoint.sh` | the container's entry: Pterodactyl's `STARTUP`, or the start script |
| `multivoid-start.sh` | starts the game under Wine, forwards stdin to the server, tails its log, turns SIGTERM into `stop` |
| `egg-multivoid.json` | the Pterodactyl egg |
| `run-dedicated.ps1` | the same on Windows, in a PowerShell console |
| `server.ini.example` | the settings file, for `Mods\Multivoid\server.ini` |

## Windows

```powershell
.\server-docker\run-dedicated.ps1 -GameDir "D:\Games\VotV" -SaveSlot myworld -Port 47621 -Name "Our world"
```

Type `help` in that console. `stop` (or Ctrl+C) saves and exits. Forward UDP 47621 for a direct
server, or pass `-Reach brokered`.

## Docker

```bash
docker build -t multivoid-server server-docker
mkdir -p data && cp -r "/path/to/your/VotV" data/VotV      # your own game folder
docker run -it --rm -p 47621:47621/udp -v "$PWD/data:/home/container" \
    -e SERVER_PORT=47621 -e SAVE_SLOT=myworld -e SERVER_NAME="Our world" multivoid-server
```

`-it` gives the server a console: type commands into it. `docker stop` sends SIGTERM, which the
start script turns into `stop`; give it time (`docker stop -t 120`). The world lives in
`data/.wine/drive_c/users/container/AppData/Local/VotV/Saved/SaveGames/`.

Variables: `SERVER_PORT`, `SERVER_NAME`, `PASSWORD`, `MAX_PLAYERS`, `SAVE_SLOT`, `LIST_PUBLIC`,
`REACH`, `AUTOSAVE_MINUTES`, `USE_XVFB` (default 1), `RHI` (`null` default, `software`), `LOG_MODE`
(`tail` default, `stdout`), `STOP_TIMEOUT`, `EXTRA_ARGS`, `GAME_DIR`.

## Pterodactyl

1. Build the image and push it where your nodes can pull it; set the egg's image to that tag
   (the egg names `ghcr.io/votv-mp/multivoid-server:latest` as a placeholder).
2. Admin, Nests, Import Egg: `egg-multivoid.json`. Set the author to yours if you keep the egg.
3. Create a server with a UDP allocation. The install step only lays out folders.
4. Upload your game folder into `VotV/` (File Manager or SFTP). Optionally upload a save into
   `.wine/drive_c/users/container/AppData/Local/VotV/Saved/SaveGames/` and set `SAVE_SLOT` to its
   name without `.sav`.
5. Start. The panel shows the server as running at `Multivoid dedicated server ready`. The panel's
   stop button sends `stop`.

## Test steps

On Windows first (fewest unknowns), then Wine, then the panel.

1. **Build** the DLL as usual and install it into a game folder with UE4SS.
2. **Plain launch unchanged:** start the game normally. The log has no `[server]` line, the F1
   overlay works, hosting from the menu works.
3. **Windowed server:** `run-dedicated.ps1 -GameDir ... -SaveSlot test1 -Windowed`. Expect, in order:
   `[server] dedicated server mode -- ...`, `harness: dedicated server -- the overlay and the in-game
   console are not installed`, `[server] no save 'test1' or 's_test1' -- creating a new story save`,
   `harness: host-with-save ...`, `rig: READY hosting`, `[server] Multivoid dedicated server ready`.
   The game window shows the world, with no F1 overlay.
4. **Join** from a second PC (Direct Connect `ip:47621`, or the browser if listed). Type `players`
   (two rows), `say hello` (the chat line on the client), `kick <nick>` (the client is kicked).
5. **Save and stop:** `save` -> `[server] world saved` and `s_test1.sav` updated. `stop` ->
   `[server] stopping`, `[server] world saved`, `[server] stopped`, then the process exits. Start
   again with `-SaveSlot test1`: `[server] hosting the save 's_test1'`.
6. **Headless:** the same without `-Windowed` (`-nullrhi`). Watch for the world never reaching
   gameplay (no `rig: READY hosting` within the timeout -> exit code 3) and say which line the log
   stops at.
7. **Autosave:** `-AutosaveMinutes 1`, wait two minutes: two `[server] autosave` lines.
8. **Wine:** the Docker run above, `LOG_MODE=tail`, then `RHI=software` if step 6's failure shows
   up only under Wine; `USE_XVFB=0` to learn whether a display is needed at all.
9. **Panel:** import the egg, upload, start; the done-detection, the console commands and the stop
   button.
