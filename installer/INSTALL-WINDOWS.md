# Multivoid on Windows: install guide

Multivoid is a co-op mod for **Voices of the Void 0.10.0**. This guide is for installing it on Windows.
(On Linux with Proton, see `INSTALL-LINUX.md`.)

## What you need

- **Voices of the Void 0.10.0** (the itch.io or Steam-less build you already play). Other versions do not work;
  the installer tells you if your copy is a different one.
- This folder, **unzipped** (not run from inside the zip). Keep the `payload` folder next to the installer.
- Nothing else. No UE4SS, no mod manager, no internet.

## Install

1. Close the game.
2. Double-click **`Install-Multivoid.bat`**.
3. Press **1** (Install). The installer looks for the game by itself; if it cannot find it, a folder picker opens:
   choose the folder that contains `WindowsNoEditor`.
   It remembers where the game is (in `%LOCALAPPDATA%\Multivoid\installer-cache.json`), so later runs skip the
   search. If the game moved, it notices and searches again. To make it forget and search from scratch, press **R**
   in the menu, or run `Install-Multivoid.bat -Rescan`.
4. Start the game. The main menu now has a **Multiplayer** entry showing the Multivoid version.

If Windows says the files are blocked (downloaded from the internet): right-click the zip, Properties, tick
**Unblock**, unzip again.

## Update

Unzip the new installer and run **1) Install** again. It replaces the changed files and keeps your settings.
If this exact version is already installed it says "already up to date" and asks whether to reinstall anyway.

## Uninstall

Run `Install-Multivoid.bat` and press **2** (Uninstall). It removes exactly what it installed, puts back anything it
backed up (for example UE4SS), and asks before deleting your settings, saves, identity key or logs. The default is to
keep them. The game then starts stock again.

## Repair

Run `Install-Multivoid.bat` and press **3** (Repair). It re-checks every installed file and restores the missing or
changed ones. Use it after a game update, an antivirus clean-up, or a "no Multiplayer entry" problem.

## If you already use UE4SS or another mod's `dwmapi.dll`

The installer never overwrites it silently. It asks:

- **Keep**: your UE4SS stays; Multivoid starts through it.
- **Replace**: UE4SS and that `dwmapi.dll` are renamed `*.multivoid-backup` and stay off until you uninstall Multivoid.

## Troubleshooting

- **No Multiplayer entry in the main menu**: open `VotV\Binaries\Win64\Mods\Multivoid\loader.log` (inside the game
  folder). It says whether the loader ran, whether `main.dll` loaded, and whether the mod started or refused. Then run
  **3) Repair**.
- **"This is NOT the VOTV 0.10.0 build"**: your game is another version. Update or downgrade the game, or the mod will
  refuse to start.
- **The installer says the game is running**: close it (check Task Manager for `VotV-Win64-Shipping.exe`).
- **Antivirus removed `dwmapi.dll`**: add an exception for the game folder, then run Repair.
- The mod's own log is `multivoid.log` beside `VotV-Win64-Shipping.exe`. The in-game F1 menu has a bug-report button.
- Still stuck: ask on the Discord linked from the main README and attach `loader.log` and `multivoid.log`.
