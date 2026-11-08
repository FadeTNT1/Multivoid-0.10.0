# Multivoid on macOS (CrossOver / Wine wrappers): install guide

Multivoid is a co-op mod for **Voices of the Void 0.10.0**. The game has no Mac build, so Mac players run the Windows
build inside a Wine wrapper: **CrossOver** (the main option), Whisky, Sikarugir / Kegworks / Wineskin wrappers, or
plain Wine / the Game Porting Toolkit. (Windows players: see `INSTALL-WINDOWS.md`. Linux: `INSTALL-LINUX.md`.)

**Status: community support, untested by the Multivoid authors.** The installer logic (finding the game inside a
bottle, editing the Wine registry, uninstalling) is tested against a fake prefix only. Nobody has yet verified a full
run on a real Mac. If it works or breaks for you, please tell us on the Discord linked from the main README and attach
`loader.log` and `multivoid.log`.

## What you need

- **Voices of the Void 0.10.0** (the Windows build) already **running** in a Wine wrapper. Get it working first; this
  installer does not set up Wine.
- CrossOver (CodeWeavers lists VOTV for macOS 15 / CrossOver 25) or another wrapper. On Apple Silicon, CrossOver needs
  **Rosetta 2** to run x86 Windows games (`softwareupdate --install-rosetta` if macOS has not installed it yet).
- This folder, **unzipped**. Keep the `payload` folder next to `install-multivoid.sh`.
- Nothing else: macOS ships bash, `find`, `grep`, `sed`, `awk`, `od` and `shasum`. Nothing is downloaded.

## Install

1. **Quit the game, and quit CrossOver / Whisky / the wrapper completely** (not just the game window). Wine rewrites
   its settings when it exits; if it is still running, the installer cannot set the DLL override safely.
2. Start the installer, either way:
   - Double-click **`Install-Multivoid-Mac.command`**. The first time macOS may refuse ("cannot be opened because it is
     from an unidentified developer"): right-click the file, choose **Open**, then **Open** again. The launcher clears
     the download quarantine flag on this folder and runs the installer in Terminal.
   - Or in Terminal, in this folder: `bash install-multivoid.sh`
3. Press **1** (Install). It looks inside your CrossOver bottles
   (`~/Library/Application Support/CrossOver/Bottles/*`), Whisky bottles, Sikarugir / Kegworks / Wineskin wrappers
   (`~/Applications/...`), `~/.wine`, and `~/Games`, `~/Downloads`, `~/Desktop`, `~/Documents` (macOS may ask whether
   Terminal may access those folders). If it finds several copies it lists them and marks the 0.10.0 one. If it finds
   none, paste the game folder (the one containing `WindowsNoEditor`); you can drag the folder into the Terminal window.
4. **DLL override.** Wine uses its own built-in `dwmapi.dll` unless told otherwise, and without the override the mod
   never starts. When the game sits inside a Wine prefix (a bottle with `drive_c` and `user.reg`), the installer offers
   to add `"dwmapi"="native,builtin"` to that bottle's `user.reg` for you (default yes). It makes a backup first
   (`user.reg.multivoid-backup`). Uninstall removes the line again (or restores the previous value).

   If it could not (Wine still running, `--no-dll-override`, or the game is outside a bottle), set it by hand with
   the matching steps below.
5. Start the game from your wrapper. The main menu now has a **Multiplayer** entry showing the Multivoid version.

### Setting the override by hand

Do this with the bottle closed first, then start the game.

- **CrossOver**: select the bottle, **Wine Configuration** (winecfg), **Libraries** tab, type `dwmapi` in "New override
  for library", **Add**, select it, **Edit**, choose **Native then Builtin**, OK.
- **Whisky**: select the bottle, **Config**, **Open Wine Configuration**, Libraries tab, same steps as above.
- **Sikarugir / Kegworks / Wineskin**: open the wrapper app, **Tools** (Wineskin: Advanced, Tools), **Config Utility
  (winecfg)**, Libraries tab, same steps.
- **Plain Wine / Game Porting Toolkit**: `WINEDLLOVERRIDES="dwmapi=n,b" wine VotV-Win64-Shipping.exe`

Unattended: `bash install-multivoid.sh --action install --game-path "$HOME/Library/Application Support/CrossOver/Bottles/MyBottle/drive_c/Games/VotV" --yes`
(`--no-dll-override` skips the registry edit; see `--help` for all options).

## In-game overlay (chat, F1 menu)

Under Apple's D3DMetal (the Game Porting Toolkit graphics layer that CrossOver uses by default) the mod's in-game
overlay may not draw. This is untested. If the Multiplayer menu entry shows but chat or the F1 menu are missing:

- Make the game use DirectX 11: in CrossOver use **Run Command** on the game exe and add `-dx11` after the path, or
  add `-dx11` to the launcher/shortcut command.
- In the CrossOver bottle settings, try the graphics backend toggle (DXVK on, D3DMetal off), then restart the bottle.

## Update

Unzip the new installer and run **1) Install** again (with Wine quit). Your settings stay.

## Uninstall

Quit Wine, run the installer, press **2**. It removes exactly what it installed, restores anything it backed up (for
example UE4SS), takes the `dwmapi` override back out of the bottle's `user.reg` (if the installer had added it), and asks
before deleting your settings, saves, identity key or logs (default: keep). `user.reg.multivoid-backup` is left behind;
delete it when you like.

## Repair

Press **3**. Re-checks every installed file, restores missing or changed ones, and re-applies the DLL override if it
has gone missing.

## If you already use UE4SS

The installer asks whether to **Keep** UE4SS (Multivoid then starts through it; the installer does not touch the
registry then) or **Replace** it (UE4SS is renamed `*.multivoid-backup` and comes back on uninstall).

## Troubleshooting

- **No Multiplayer entry in the main menu**: open `Mods/Multivoid/loader.log` (in `.../VotV/Binaries/Win64/` inside
  the bottle). No `loader.log` at all means Wine never loaded our `dwmapi.dll`, so the override is missing: check the
  Libraries tab of winecfg for `dwmapi` set to Native then Builtin. A log that says the loader ran but the mod refused
  tells you why.
- **The installer says Wine is running**: quit CrossOver / Whisky / the wrapper fully (also its menu-bar icon). Wine's
  `wineserver` lingers for a few seconds after the game closes; wait, then press Enter. `pgrep -fl wineserver` shows
  what is still alive.
- **The `.command` file will not open**: right-click, **Open**. Or run `xattr -dr com.apple.quarantine <folder>` and
  `bash install-multivoid.sh` in Terminal.
- **Installer cannot find the game**: pass it explicitly with `--game-path`. A game stored outside the bottle (reached
  through Wine's `Z:` drive) works, but then the DLL override has to be set by hand.
- **"This is NOT the VOTV 0.10.0 build"**: your game is another version; the mod refuses to run on it.
- The mod's own log is `multivoid.log` beside `VotV-Win64-Shipping.exe`.
- Still stuck: ask on the Discord linked from the main README and attach `loader.log` and `multivoid.log`.
