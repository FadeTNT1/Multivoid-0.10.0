# Multivoid on Linux (Proton / Wine): install guide

Multivoid is a co-op mod for **Voices of the Void 0.10.0**. This guide is for players who run the Windows build
through Proton or Wine. Most people add the itch.io build to Steam as a **non-Steam game**; Lutris, Heroic and Bottles
work too. (Windows players: see `INSTALL-WINDOWS.md`.)

## What you need

- **Voices of the Void 0.10.0** (the Windows build, extracted somewhere on your disk) already running under Proton/Wine.
- This folder, **unzipped**. Keep the `payload` folder next to `install-multivoid.sh`.
- `bash` and the usual tools (`find`, `grep`, `sed`, `od`, `sha256sum`). Any normal distro has them. Nothing is downloaded.

## Install

1. Close the game (and Steam's game session for it).
2. Open a terminal in this folder and run:

   ```
   chmod +x install-multivoid.sh
   ./install-multivoid.sh
   ```

   (Or `bash install-multivoid.sh` if the zip lost the executable bit.) Double-clicking in a file manager only works
   if it offers "Run in terminal"; a terminal is the reliable way.
3. Press **1** (Install). It looks for the game in `~/Games`, `~/Downloads`, `~/Desktop`, your Steam libraries
   (including Flatpak Steam), Lutris and Heroic folders. If it finds several copies it lists them and marks the 0.10.0
   one. If it finds none, paste the game folder (the one that contains `WindowsNoEditor`).
4. **Set the launch option. This step is required.** Wine uses its own built-in `dwmapi.dll` unless told otherwise, and
   without this the mod never starts.

   - **Steam** (non-Steam shortcut): Library, right-click the game, Properties, **Launch Options**, paste:

     ```
     WINEDLLOVERRIDES="dwmapi=n,b" %command%
     ```

     Steam is not edited by the installer. Flatpak Steam: same line. (VOTV is not on Steam, so this is always a
     non-Steam shortcut.)
   - **Lutris**: game, Configure, System options, Environment variables: `WINEDLLOVERRIDES` = `dwmapi=n,b`.
   - **Heroic**: game, Settings, Advanced, Environment Variables: `WINEDLLOVERRIDES` = `dwmapi=n,b`.
   - **Bottles**: bottle Settings, DLL overrides, add `dwmapi` as *Native, Builtin*.
5. Start the game. The main menu now has a **Multiplayer** entry showing the Multivoid version.

Unattended: `./install-multivoid.sh --action install --game-path "$HOME/Games/VotV" --yes`
(see `./install-multivoid.sh --help` for all options).

## Update

Unzip the new installer and run **1) Install** again. Your settings stay.

## Uninstall

`./install-multivoid.sh`, press **2**. It removes exactly what it installed, restores anything it backed up (for
example UE4SS), and asks before deleting your settings, saves, identity key or logs (default: keep). You can then remove
the `WINEDLLOVERRIDES` launch option.

## Repair

`./install-multivoid.sh`, press **3**. Re-checks every installed file and restores the missing or changed ones.

## If you already use UE4SS

The installer asks whether to **Keep** UE4SS (Multivoid then starts through it; use the launch option from
`docs/install.md`: `WINEDLLOVERRIDES="dwmapi.dll,main.dll,UE4SS=n,b" %command%`) or **Replace** it (UE4SS is renamed
`*.multivoid-backup` and comes back on uninstall).

## Troubleshooting

- **No Multiplayer entry in the main menu**: first check the launch option from step 4 is really set. Then open
  `Mods/Multivoid/loader.log` (in `.../VotV/Binaries/Win64/`). No `loader.log` at all means Wine never loaded our
  `dwmapi.dll`, so the override is missing or mistyped. A log that says the loader ran but the mod refused tells you why.
- **Proton version**: use a recent Proton (stock or Proton-GE). If the game starts but the overlay (chat, F1 menu) is
  missing, try another Proton version, or start the game on DirectX 11 by adding `-dx11` after `%command%`.
  (Not every Proton version has been tested with this installer.)
- **"This is NOT the VOTV 0.10.0 build"**: your game is another version; the mod refuses to run on it.
- **Installer cannot find the game**: pass it explicitly: `./install-multivoid.sh --game-path /path/to/VotV`.
- **Flatpak Steam**: the game folder is wherever you extracted it (it must be readable by the Flatpak). Steam's own data
  lives under `~/.var/app/com.valvesoftware.Steam/`. If the game is outside your home folder, grant Flatpak access
  (`flatpak override --user --filesystem=/path com.valvesoftware.Steam`).
- **Installer says the game is running**: close the game and its Steam session; `pgrep -af VotV-Win64-Shipping` shows
  what is still alive.
- The mod's own log is `multivoid.log` beside `VotV-Win64-Shipping.exe`. The in-game F1 menu has a bug-report button.
- Still stuck: ask on the Discord linked from the main README and attach `loader.log` and `multivoid.log`.
