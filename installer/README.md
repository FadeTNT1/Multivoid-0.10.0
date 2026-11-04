# Multivoid installer (for friends)

Installs the Multivoid co-op mod into **Voices of the Void 0.10.0**. No UE4SS, no mod manager, no downloads: one
small loader file (`dwmapi.dll`) goes next to the game's exe and starts the mod by itself.

## Install

1. Unzip the whole `Multivoid-Installer-0.10.0` folder somewhere (the Desktop is fine). Keep `payload\` next to the
   installer: it holds the files that get installed.
2. Close the game.
3. Double-click **`Install-Multivoid.bat`**, and pick `1) Install`.

   It finds the game itself (Steam libraries, common game folders, Desktop, Downloads). If it cannot, it opens a
   folder picker: choose the folder that contains `WindowsNoEditor` (or the one holding
   `VotV-Win64-Shipping.exe`).
4. Start the game. The main menu now has a **Multiplayer** entry showing the Multivoid version.

If Windows says the zip's files are blocked ("downloaded from the internet"), right-click the zip, Properties,
tick **Unblock**, and unzip again. The `.bat` already runs the script with the execution policy bypassed, so
nothing needs to be changed in PowerShell.

## The menu

| Choice | What it does |
| --- | --- |
| **1) Install** | Copies the mod, the loader and (if shipped) the player skins into the game; writes `Mods\Multivoid\install-manifest.json` listing every file it added with its SHA256. Safe to run again to update. |
| **2) Uninstall** | Removes exactly the files in the manifest, puts back anything it backed up, and asks before deleting your Multivoid settings, saves, identity key or logs (default: keep them). |
| **3) Repair** | Re-checks every installed file against the payload and re-copies what is missing or changed. Also moves old-style leftovers aside (`multivoid-*.dll`, `votv-coop.dll` and Multivoid's old `xinput1_3.dll` beside the exe, which stop the new build from starting). |

Unattended, from a terminal (no questions asked; every choice is a switch with a safe default):

```
Install-Multivoid.bat -Action Install   -GamePath "D:\Games\VotV"
Install-Multivoid.bat -Action Repair    -GamePath "D:\Games\VotV"
Install-Multivoid.bat -Action Uninstall -GamePath "D:\Games\VotV" -RemoveLogs
```

| Switch | Meaning |
| --- | --- |
| `-Action Install\|Uninstall\|Repair` | Skip the menu. |
| `-GamePath <path>` | The game folder, `WindowsNoEditor`, `VotV\Binaries\Win64`, or the exe. |
| `-Ue4ss Ask\|Keep\|Replace` | What to do about an existing UE4SS (see below). Unattended `Ask` means `Keep`. |
| `-SkipVersionCheck` | Go on although the exe is not the 0.10.0 build. |
| `-RemoveLogs` / `-RemoveUserData` | Uninstall only: also delete logs and reports / settings, saves and identity key. |
| `-PayloadDir <path>` | Use a payload folder somewhere else. |
| `-NonInteractive` | Never ask a question. |

## What it checks

- **Game version.** The mod is made for VOTV **0.10.0** only. The installer reads the build stamp of
  `VotV-Win64-Shipping.exe` and warns (and asks) if it is another version.
- **The game must be closed.** If it is running, the installer offers to close it.
- **UE4SS / another `dwmapi.dll`.** If UE4SS (or another mod's `dwmapi.dll`) is already there, the installer
  never overwrites it silently. You choose:
  - **Keep**: your UE4SS stays the loader, the Multivoid loader is *not* installed, and the mod starts through
    UE4SS (it still works there).
  - **Replace**: UE4SS and that `dwmapi.dll` are renamed `*.multivoid-backup`. UE4SS and its other mods stay **off**
    until you uninstall Multivoid, which puts them back.

  The Multivoid loader deliberately does nothing when it sees UE4SS, so it never starts the mod twice.

## What gets installed, and where

Inside `<game>\WindowsNoEditor\VotV`:

| Payload | Goes to |
| --- | --- |
| `payload\mod\` | `Binaries\Win64\Mods\Multivoid\` (`dlls\main.dll`, `enabled.txt`, ...) |
| `payload\loader\dwmapi.dll` | `Binaries\Win64\dwmapi.dll` (beside the exe) |
| `payload\pak\` (optional) | `Content\Paks\LogicMods\multivoid\` (player skins) |

The mod's own files appear later in the same `Mods\Multivoid` folder: `multivoid.ini`, saved inventories, the
identity key, and **`loader.log`**, which records how the loader started the mod.

## If something is wrong

- Nothing happens in game / no Multiplayer entry: open `Mods\Multivoid\loader.log`. It says whether the loader ran,
  whether it stood aside for UE4SS, whether `main.dll` loaded, and whether the mod started or refused.
  Then try **3) Repair**.
- Linux / Steam Proton: set the launch option `WINEDLLOVERRIDES="dwmapi=n,b" %command%` so Wine uses the
  `dwmapi.dll` beside the game instead of its built-in one.
- The mod's own log is `multivoid.log` beside the game exe; the F1 menu has a bug-report button.
- Still stuck: the Discord linked from the main README.

## For whoever builds the package

`installer\Build-Package.ps1` assembles `dist\Multivoid-Installer-0.10.0\` and `.zip` from
`build\votv-coop\Release\main.dll` and `dwmapi.dll` (the `multivoid-loader` CMake target) plus `assets\paks` when
present. `-PackageZip <release zip>` takes the release-signed `mod\` and `pak\` from the release package instead.
