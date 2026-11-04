<#
.SYNOPSIS
    Multivoid friend installer: install, uninstall or repair the Multivoid co-op mod for Voices of the Void 0.10.0.

.DESCRIPTION
    Run it from the folder it came in (it needs the payload\ folder beside it):
        payload\mod\            -> <game>\VotV\Binaries\Win64\Mods\Multivoid\   (dlls\main.dll, enabled.txt, ...)
        payload\loader\dwmapi.dll -> <game>\VotV\Binaries\Win64\dwmapi.dll       (the Multivoid loader)
        payload\pak\            -> <game>\VotV\Content\Paks\LogicMods\multivoid\ (optional skins)

    No UE4SS is needed: dwmapi.dll starts the mod by itself. Nothing is downloaded. Windows PowerShell 5.1 is enough.

    Interactive: just run it and pick from the menu. Unattended: pass -Action and -GamePath (every question is then
    answered by a switch, with the safe answer as the default).

.PARAMETER Action
    Install, Uninstall or Repair. Without it a menu is shown.

.PARAMETER GamePath
    The game folder (the one holding WindowsNoEditor), the WindowsNoEditor folder, VotV\Binaries\Win64, or the
    VotV-Win64-Shipping.exe itself. Without it the installer looks for the game and asks.

.PARAMETER PayloadDir
    Where the payload folder is. Default: payload\ beside this script.

.PARAMETER Ue4ss
    What to do when UE4SS (or another mod's dwmapi.dll) is already installed. Keep: leave it as the loader and do
    not install the Multivoid loader (the mod then starts through UE4SS). Replace: back UE4SS and that dwmapi.dll up
    (they come back on uninstall) and install the Multivoid loader. Ask (default): ask; unattended it means Keep.

.PARAMETER SkipVersionCheck
    Go on even if the game's exe is not the VOTV 0.10.0 build. Unattended runs stop on a mismatch without it.

.PARAMETER RemoveLogs
    Uninstall only, unattended: also delete the mod's logs and saved bug reports.

.PARAMETER RemoveUserData
    Uninstall only, unattended: also delete the mod's settings, saved inventories, translations and identity key.

.PARAMETER NonInteractive
    Never ask a question (implied when both -Action and -GamePath are given).
#>
[CmdletBinding()]
param(
    [ValidateSet('Install', 'Uninstall', 'Repair')]
    [string]$Action,
    [string]$GamePath,
    [string]$PayloadDir,
    [ValidateSet('Ask', 'Keep', 'Replace')]
    [string]$Ue4ss = 'Ask',
    [switch]$SkipVersionCheck,
    [switch]$RemoveLogs,
    [switch]$RemoveUserData,
    [switch]$NonInteractive
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------------------------------------------
$script:InstallerVersion = '1.0'
$script:GameTarget       = '0.10.0'
$script:ExpectedStamp    = [uint32]0x6ae37b05          # PE TimeDateStamp of VotV-Win64-Shipping.exe, 0.10.0
$script:ExeName          = 'VotV-Win64-Shipping.exe'
$script:ManifestName     = 'install-manifest.json'
$script:BackupSuffix     = '.multivoid-backup'
$script:OldLayoutDir     = 'multivoid-old-install-backup'
$script:Interactive      = -not ($NonInteractive -or ($PSBoundParameters.ContainsKey('Action') -and $PSBoundParameters.ContainsKey('GamePath')))

# ---------------------------------------------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------------------------------------------
function Write-Tag([string]$Tag, [string]$Message, [string]$Color) {
    Write-Host ('{0,-7}' -f $Tag) -ForegroundColor $Color -NoNewline
    Write-Host $Message
}
function Write-Step([string]$Message) { Write-Host ''; Write-Host $Message -ForegroundColor Cyan }
function Write-Info([string]$Message) { Write-Tag '[..]'   $Message 'Gray' }
function Write-Ok([string]$Message)   { Write-Tag '[ OK ]' $Message 'Green' }
function Write-Warn([string]$Message) { Write-Tag '[WARN]' $Message 'Yellow' }
function Write-Fail([string]$Message) { Write-Tag '[FAIL]' $Message 'Red' }

function Stop-Installer([string]$Message) {
    # One exception type, caught at the bottom: a clean message and exit code 1, never a stack trace.
    throw (New-Object System.InvalidOperationException($Message))
}

function Read-YesNo([string]$Question, [bool]$Default) {
    if (-not $script:Interactive) { return $Default }
    $hint = if ($Default) { 'Y/n' } else { 'y/N' }
    while ($true) {
        $a = (Read-Host ("{0} [{1}]" -f $Question, $hint)).Trim().ToLowerInvariant()
        if ($a -eq '') { return $Default }
        if ($a -in @('y', 'yes')) { return $true }
        if ($a -in @('n', 'no')) { return $false }
        Write-Host 'Please answer y or n.' -ForegroundColor Yellow
    }
}

# ---------------------------------------------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------------------------------------------
function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-PeTimestamp([string]$Exe) {
    $fs = [System.IO.File]::OpenRead($Exe)
    try {
        $br = New-Object System.IO.BinaryReader($fs)
        $fs.Seek(0x3C, [System.IO.SeekOrigin]::Begin) | Out-Null
        $peOffset = $br.ReadInt32()
        $fs.Seek($peOffset, [System.IO.SeekOrigin]::Begin) | Out-Null
        if ($br.ReadUInt32() -ne [uint32]0x00004550) { return $null }   # "PE\0\0"
        $br.ReadUInt16() | Out-Null                                      # Machine
        $br.ReadUInt16() | Out-Null                                      # NumberOfSections
        return $br.ReadUInt32()                                          # TimeDateStamp
    } finally { $fs.Dispose() }
}

function Test-FileContains([string]$Path, [byte[]]$Needle) {
    # Byte search through a Latin-1 view of the file (1 char == 1 byte, so IndexOf is a byte search).
    $latin1 = [System.Text.Encoding]::GetEncoding(28591)
    $hay = $latin1.GetString([System.IO.File]::ReadAllBytes($Path))
    return ($hay.IndexOf($latin1.GetString($Needle), [System.StringComparison]::Ordinal) -ge 0)
}

function Test-IsMultivoidLoader([string]$Path) {
    # Our dwmapi.dll carries its own mod path as a wide string.
    return (Test-FileContains $Path ([System.Text.Encoding]::Unicode.GetBytes('\Mods\Multivoid\dlls\main.dll')))
}

function Test-IsMultivoidXinput([string]$Path) {
    # The retired xinput1_3.dll loader carried these names.
    return ((Test-FileContains $Path ([System.Text.Encoding]::ASCII.GetBytes('MULTIVOID_DUP_FILES'))) -or
            (Test-FileContains $Path ([System.Text.Encoding]::Unicode.GetBytes('multivoid-*.dll'))))
}

function Get-RelativePath([string]$Root, [string]$Full) {
    return $Full.Substring($Root.Length).TrimStart('\', '/')
}

function Remove-EmptyDirs([string]$Dir) {
    # Bottom-up; never removes $Dir itself unless it ends up empty too.
    if (-not (Test-Path -LiteralPath $Dir -PathType Container)) { return }
    foreach ($sub in @(Get-ChildItem -LiteralPath $Dir -Directory -Force)) { Remove-EmptyDirs $sub.FullName }
    if (@(Get-ChildItem -LiteralPath $Dir -Force).Count -eq 0) { Remove-Item -LiteralPath $Dir -Force }
}

# ---------------------------------------------------------------------------------------------------------------
# Finding the game
# ---------------------------------------------------------------------------------------------------------------
function Resolve-Win64Dir([string]$Path) {
    # Any of: the exe, Win64, VotV, WindowsNoEditor, or the folder above it -> the Win64 folder holding the exe.
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    $p = $Path.Trim().Trim('"')
    if (Test-Path -LiteralPath $p -PathType Leaf) {
        if ((Split-Path -Leaf $p) -ieq $script:ExeName) { return (Split-Path -Parent (Get-Item -LiteralPath $p).FullName) }
        return $null
    }
    if (-not (Test-Path -LiteralPath $p -PathType Container)) { return $null }
    foreach ($rel in @('', 'Binaries\Win64', 'VotV\Binaries\Win64', 'WindowsNoEditor\VotV\Binaries\Win64')) {
        $c = if ($rel) { Join-Path $p $rel } else { $p }
        if (Test-Path -LiteralPath (Join-Path $c $script:ExeName) -PathType Leaf) { return (Get-Item -LiteralPath $c).FullName }
    }
    return $null
}

function Get-SteamLibraries {
    $libs = New-Object System.Collections.Generic.List[string]
    $steam = $null
    try { $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath } catch { $steam = $null }
    $roots = New-Object System.Collections.Generic.List[string]
    if ($steam) { $roots.Add(($steam -replace '/', '\')) }
    foreach ($pf in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if ($pf) { $roots.Add((Join-Path $pf 'Steam')) }
    }
    foreach ($r in $roots) {
        if (-not (Test-Path -LiteralPath $r -PathType Container)) { continue }
        if (-not $libs.Contains($r)) { $libs.Add($r) }
        $vdf = Join-Path $r 'steamapps\libraryfolders.vdf'
        if (-not (Test-Path -LiteralPath $vdf -PathType Leaf)) { continue }
        foreach ($line in @(Get-Content -LiteralPath $vdf -ErrorAction SilentlyContinue)) {
            if ($line -match '^\s*"path"\s+"(.+)"\s*$') {
                $lib = ($Matches[1] -replace '\\\\', '\')
                if ((Test-Path -LiteralPath $lib -PathType Container) -and -not $libs.Contains($lib)) { $libs.Add($lib) }
            }
        }
    }
    return $libs.ToArray()
}

function Find-GameCandidates {
    $found = New-Object System.Collections.Generic.List[string]
    $add = {
        param([string]$dir)
        if ($dir -and -not ($found | Where-Object { $_ -ieq $dir })) { $found.Add($dir) }
    }

    Write-Info 'Looking in your Steam libraries...'
    foreach ($lib in (Get-SteamLibraries)) {
        $common = Join-Path $lib 'steamapps\common'
        if (-not (Test-Path -LiteralPath $common -PathType Container)) { continue }
        foreach ($d in @(Get-ChildItem -LiteralPath $common -Directory -ErrorAction SilentlyContinue |
                         Where-Object { $_.Name -match 'void|votv' })) {
            $w = Resolve-Win64Dir $d.FullName
            if ($w) { & $add $w }
        }
    }

    Write-Info 'Looking in the usual game folders...'
    $roots = New-Object System.Collections.Generic.List[string]
    foreach ($drive in [System.IO.DriveInfo]::GetDrives()) {
        if ($drive.DriveType -ne [System.IO.DriveType]::Fixed -or -not $drive.IsReady) { continue }
        foreach ($rel in @('Games', 'Program Files (x86)\Steam\steamapps\common', 'SteamLibrary\steamapps\common', 'Program Files\Steam\steamapps\common')) {
            $roots.Add((Join-Path $drive.RootDirectory.FullName $rel))
        }
    }
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        foreach ($f in @(Get-ChildItem -LiteralPath $root -Recurse -Depth 6 -Filter $script:ExeName -File -ErrorAction SilentlyContinue)) {
            $w = Resolve-Win64Dir $f.DirectoryName
            if ($w) { & $add $w }
        }
    }

    Write-Info 'Looking on your Desktop and in Downloads (this can take a moment)...'
    $user = [Environment]::GetFolderPath('UserProfile')
    $personal = @([Environment]::GetFolderPath('Desktop'), (Join-Path $user 'Downloads'), (Join-Path $user 'OneDrive\Desktop'))
    foreach ($root in $personal) {
        if (-not $root -or -not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        foreach ($f in @(Get-ChildItem -LiteralPath $root -Recurse -Depth 7 -Filter $script:ExeName -File -ErrorAction SilentlyContinue)) {
            $w = Resolve-Win64Dir $f.DirectoryName
            if ($w) { & $add $w }
        }
    }
    return $found.ToArray()
}

function Show-FolderPicker {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
        $dlg.Description = 'Pick the Voices of the Void game folder (the one that contains WindowsNoEditor)'
        $dlg.ShowNewFolderButton = $false
        if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dlg.SelectedPath }
    } catch { }
    return $null
}

function Select-GameFolder {
    if ($GamePath) {
        $w = Resolve-Win64Dir $GamePath
        if (-not $w) { Stop-Installer "No $script:ExeName found under '$GamePath'. Point -GamePath at the game folder." }
        return $w
    }
    if (-not $script:Interactive) { Stop-Installer 'Unattended runs need -GamePath.' }

    # Copies that are the 0.10.0 build first; each is labelled so a friend with old copies lying around picks right.
    $cands = @(Find-GameCandidates | Sort-Object { if ((Get-PeTimestamp (Join-Path $_ $script:ExeName)) -eq $script:ExpectedStamp) { 0 } else { 1 } })
    if ($cands.Count -eq 1) {
        Write-Ok "Found the game: $($cands[0])"
        if (Read-YesNo 'Use this one?' $true) { return $cands[0] }
    } elseif ($cands.Count -gt 1) {
        Write-Ok "Found $($cands.Count) copies of the game:"
        for ($i = 0; $i -lt $cands.Count; $i++) {
            $isTarget = (Get-PeTimestamp (Join-Path $cands[$i] $script:ExeName)) -eq $script:ExpectedStamp
            Write-Host ("  {0}) {1}" -f ($i + 1), $cands[$i]) -NoNewline
            if ($isTarget) { Write-Host ("   [VOTV {0}]" -f $script:GameTarget) -ForegroundColor Green } else { Write-Host "   [another version]" -ForegroundColor DarkYellow }
        }
        while ($true) {
            $a = (Read-Host 'Pick a number (or press Enter to browse for another)').Trim()
            if ($a -eq '') { break }
            $n = 0
            if ([int]::TryParse($a, [ref]$n) -and $n -ge 1 -and $n -le $cands.Count) { return $cands[$n - 1] }
        }
    } else {
        Write-Warn 'Could not find the game by itself.'
    }
    while ($true) {
        $p = Show-FolderPicker
        if (-not $p) { $p = Read-Host 'Type or paste the game folder (or the path of VotV-Win64-Shipping.exe), empty to quit' }
        if ([string]::IsNullOrWhiteSpace($p)) { Stop-Installer 'No game folder chosen.' }
        $w = Resolve-Win64Dir $p
        if ($w) { return $w }
        Write-Warn "No $script:ExeName under '$p'. Try again."
    }
}

function Get-Layout([string]$Win64) {
    $pak = [System.IO.Path]::GetFullPath((Join-Path $Win64 '..\..\Content\Paks\LogicMods\multivoid'))
    return [pscustomobject]@{
        Win64    = $Win64
        Exe      = Join-Path $Win64 $script:ExeName
        ModDir   = Join-Path $Win64 'Mods\Multivoid'
        PakDir   = $pak
        Manifest = Join-Path $Win64 ('Mods\Multivoid\' + $script:ManifestName)
    }
}

function Resolve-Dest($Layout, [string]$Base, [string]$Rel) {
    $root = if ($Base -eq 'pak') { $Layout.PakDir } else { $Layout.Win64 }
    return (Join-Path $root $Rel)
}

function Confirm-GameVersion($Layout) {
    $stamp = Get-PeTimestamp $Layout.Exe
    if ($null -ne $stamp -and $stamp -eq $script:ExpectedStamp) {
        Write-Ok ("Game build is VOTV {0} (exe stamp 0x{1:x8})." -f $script:GameTarget, $stamp)
        return
    }
    $shown = if ($null -eq $stamp) { 'unreadable' } else { ('0x{0:x8}' -f $stamp) }
    Write-Warn ("This is NOT the VOTV {0} build this Multivoid was made for (exe stamp {1}, expected 0x{2:x8})." -f $script:GameTarget, $shown, $script:ExpectedStamp)
    Write-Warn 'On another version the mod will refuse to start, or misbehave.'
    if ($SkipVersionCheck) { Write-Warn '-SkipVersionCheck given: going on.'; return }
    if (-not $script:Interactive) { Stop-Installer 'Game version mismatch (pass -SkipVersionCheck to override).' }
    if (-not (Read-YesNo 'Continue anyway?' $false)) { Stop-Installer 'Stopped: wrong game version.' }
}

function Assert-GameClosed($Layout) {
    while ($true) {
        $running = @(Get-Process -Name ([System.IO.Path]::GetFileNameWithoutExtension($script:ExeName)) -ErrorAction SilentlyContinue |
            Where-Object {
                $path = $null
                try { $path = $_.Path } catch { $path = $null }
                (-not $path) -or ($path -ieq $Layout.Exe)
            })
        if ($running.Count -eq 0) { return }
        Write-Warn 'Voices of the Void is running. Its files are locked while it runs.'
        if (-not $script:Interactive) { Stop-Installer 'Close the game first.' }
        if (-not (Read-YesNo 'Close it now?' $true)) { Stop-Installer 'Close the game and run the installer again.' }
        foreach ($p in $running) { try { [void]$p.CloseMainWindow() } catch { } }
        $waited = 0
        while ($waited -lt 15 -and @(Get-Process -Id ($running | ForEach-Object { $_.Id }) -ErrorAction SilentlyContinue).Count -gt 0) {
            Start-Sleep -Seconds 1; $waited++
        }
        $still = @(Get-Process -Id ($running | ForEach-Object { $_.Id }) -ErrorAction SilentlyContinue)
        if ($still.Count -gt 0) {
            if (Read-YesNo 'It did not close. Force it to quit? (unsaved progress is lost)' $false) {
                foreach ($p in $still) { try { Stop-Process -Id $p.Id -Force } catch { } }
                Start-Sleep -Seconds 1
            } else { Stop-Installer 'Close the game and run the installer again.' }
        }
    }
}

# ---------------------------------------------------------------------------------------------------------------
# Payload and manifest
# ---------------------------------------------------------------------------------------------------------------
function Get-Payload {
    $root = if ($PayloadDir) { $PayloadDir } else { Join-Path $PSScriptRoot 'payload' }
    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        Stop-Installer "The payload folder is missing: $root`nRun the installer from the unzipped Multivoid installer folder."
    }
    $root = (Get-Item -LiteralPath $root).FullName
    $modDir = Join-Path $root 'mod'
    $loader = Join-Path $root 'loader\dwmapi.dll'
    $pakDir = Join-Path $root 'pak'
    foreach ($need in @('mod\dlls\main.dll', 'mod\enabled.txt', 'loader\dwmapi.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $root $need) -PathType Leaf)) {
            Stop-Installer "The payload is incomplete: $need is missing from $root"
        }
    }
    $entries = New-Object System.Collections.Generic.List[object]
    $modRoot = (Get-Item -LiteralPath $modDir).FullName
    foreach ($f in @(Get-ChildItem -LiteralPath $modDir -Recurse -File)) {
        $entries.Add([pscustomobject]@{ Base = 'exe'; Rel = ('Mods\Multivoid\' + (Get-RelativePath $modRoot $f.FullName)); Src = $f.FullName; Kind = 'mod' })
    }
    if (Test-Path -LiteralPath $pakDir -PathType Container) {
        $pakRoot = (Get-Item -LiteralPath $pakDir).FullName
        foreach ($f in @(Get-ChildItem -LiteralPath $pakDir -Recurse -File)) {
            $entries.Add([pscustomobject]@{ Base = 'pak'; Rel = (Get-RelativePath $pakRoot $f.FullName); Src = $f.FullName; Kind = 'pak' })
        }
    }
    return [pscustomobject]@{
        Root    = $root
        Files   = $entries.ToArray()
        Loader  = [pscustomobject]@{ Base = 'exe'; Rel = 'dwmapi.dll'; Src = $loader; Kind = 'loader' }
    }
}

function Read-Manifest($Layout) {
    if (-not (Test-Path -LiteralPath $Layout.Manifest -PathType Leaf)) { return $null }
    try {
        return (Get-Content -LiteralPath $Layout.Manifest -Raw | ConvertFrom-Json)
    } catch {
        Write-Warn "The install manifest is unreadable ($($_.Exception.Message)); treating this as a fresh install."
        return $null
    }
}

function Write-Manifest($Layout, [object[]]$Files, [object[]]$Backups, [bool]$LoaderInstalled, [object[]]$OldLayoutMoved) {
    $doc = [ordered]@{
        schema      = 1
        installer   = ('Multivoid Installer ' + $script:InstallerVersion)
        gameTarget  = $script:GameTarget
        installedAt = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        gamePath    = $Layout.Win64
        loader      = $LoaderInstalled
        files       = @($Files)
        backups     = @($Backups)
        oldLayoutMoved = @($OldLayoutMoved)
    }
    $json = ConvertTo-Json -InputObject $doc -Depth 6
    $dir = Split-Path -Parent $Layout.Manifest
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    [System.IO.File]::WriteAllText($Layout.Manifest, $json, (New-Object System.Text.UTF8Encoding($false)))
}

function Get-ManifestArray($Manifest, [string]$Name) {
    if ($null -eq $Manifest) { return @() }
    $prop = $Manifest.PSObject.Properties[$Name]
    if ($null -eq $prop -or $null -eq $prop.Value) { return @() }
    return @($prop.Value)
}

# ---------------------------------------------------------------------------------------------------------------
# Install / Repair
# ---------------------------------------------------------------------------------------------------------------
function Find-Ue4ssFiles($Layout) {
    $hits = New-Object System.Collections.Generic.List[string]
    foreach ($rel in @('UE4SS.dll', 'ue4ss\UE4SS.dll')) {
        if (Test-Path -LiteralPath (Join-Path $Layout.Win64 $rel) -PathType Leaf) { $hits.Add($rel) }
    }
    return $hits.ToArray()
}

function New-BackupName([string]$FullPath) {
    $name = $FullPath + $script:BackupSuffix
    $i = 2
    while (Test-Path -LiteralPath $name) { $name = $FullPath + $script:BackupSuffix + $i; $i++ }
    return $name
}

function Resolve-LoaderPlan($Layout, $Payload, $OldManifest, [bool]$Repair) {
    # Decide whether the Multivoid loader goes in, and what has to be backed up for it. Touches nothing.
    $dwmPath   = Join-Path $Layout.Win64 'dwmapi.dll'
    $dwmExists = Test-Path -LiteralPath $dwmPath -PathType Leaf
    $dwmOurs   = $false
    if ($dwmExists) {
        $dwmOurs = (Get-Sha256 $dwmPath) -eq (Get-Sha256 $Payload.Loader.Src) -or (Test-IsMultivoidLoader $dwmPath)
    }
    $ue4ssFiles = @(Find-Ue4ssFiles $Layout)
    $plan = [pscustomobject]@{ Install = $true; Backups = @() }

    if ($ue4ssFiles.Count -eq 0 -and (-not $dwmExists -or $dwmOurs)) { return $plan }   # clean: no conflict

    if ($ue4ssFiles.Count -gt 0) { Write-Warn ('UE4SS is installed here (' + ($ue4ssFiles -join ', ') + ').') }
    if ($dwmExists -and -not $dwmOurs) { Write-Warn 'There is already a dwmapi.dll beside the game exe that is not Multivoid''s (UE4SS''s loader, or another mod''s).' }

    $mode = $Ue4ss
    if ($mode -eq 'Ask' -and $Repair -and $null -ne $OldManifest -and $ue4ssFiles.Count -gt 0) {
        $wasLoader = $true
        if ($OldManifest.PSObject.Properties['loader']) { $wasLoader = [bool]$OldManifest.loader }
        # Only a recorded Keep is inherited. A UE4SS that came back after a Replace is a new situation: ask again.
        if (-not $wasLoader) {
            $mode = 'Keep'
            Write-Info 'Repair keeps the choice made at install time: Keep UE4SS as the loader.'
        }
    }
    if ($mode -eq 'Ask') {
        if (-not $script:Interactive) {
            $mode = 'Keep'
        } else {
            Write-Host ''
            Write-Host '  K) Keep UE4SS as the loader. Multivoid does not install its own dwmapi.dll and starts through UE4SS.'
            Write-Host '     Nothing of yours is touched. (You need a UE4SS that works with the game.)'
            Write-Host '  R) Replace it. UE4SS and that dwmapi.dll are renamed *.multivoid-backup, so UE4SS and its other mods'
            Write-Host '     stay OFF until you uninstall Multivoid (uninstall puts them back).'
            while ($true) {
                $a = (Read-Host 'Keep UE4SS or Replace it? [K/R]').Trim().ToLowerInvariant()
                if ($a -in @('k', 'keep')) { $mode = 'Keep'; break }
                if ($a -in @('r', 'replace')) { $mode = 'Replace'; break }
            }
        }
    }
    if ($mode -eq 'Keep') {
        $plan.Install = $false
        Write-Warn 'Not installing the Multivoid loader: the mod will start through the existing loader.'
        return $plan
    }
    $backups = New-Object System.Collections.Generic.List[object]
    if ($dwmExists -and -not $dwmOurs) { $backups.Add([pscustomobject]@{ Original = 'dwmapi.dll'; Sha256 = (Get-Sha256 $dwmPath) }) }
    foreach ($u in $ue4ssFiles) { $backups.Add([pscustomobject]@{ Original = $u; Sha256 = (Get-Sha256 (Join-Path $Layout.Win64 $u)) }) }
    $plan.Backups = $backups.ToArray()
    return $plan
}

function Move-OldLayoutLeftovers($Layout) {
    # The pre-mod-folder builds: multivoid-*.dll / votv-coop.dll beside the exe, and their xinput1_3.dll loader. The
    # current mod refuses to start beside them (docs/install.md, Upgrading), so they go -- moved aside, not deleted.
    $moved = New-Object System.Collections.Generic.List[object]
    $dlls = @(Get-ChildItem -LiteralPath $Layout.Win64 -File -ErrorAction SilentlyContinue |
              Where-Object { $_.Name -like 'multivoid-*.dll' -or $_.Name -ieq 'votv-coop.dll' })
    $xin = Join-Path $Layout.Win64 'xinput1_3.dll'
    $xinOurs = $false
    if (Test-Path -LiteralPath $xin -PathType Leaf) { $xinOurs = Test-IsMultivoidXinput $xin }
    $victims = New-Object System.Collections.Generic.List[string]
    foreach ($d in $dlls) { $victims.Add($d.FullName) }
    if ($xinOurs -or ($dlls.Count -gt 0 -and (Test-Path -LiteralPath $xin -PathType Leaf))) { $victims.Add($xin) }
    elseif (Test-Path -LiteralPath $xin -PathType Leaf) {
        Write-Warn 'xinput1_3.dll beside the exe does not look like Multivoid''s, so it is left alone.'
    }
    if ($victims.Count -eq 0) { return @() }
    Write-Warn ('Old Multivoid install files found beside the exe: ' + (($victims | ForEach-Object { Split-Path -Leaf $_ }) -join ', '))
    $aside = Join-Path $Layout.Win64 $script:OldLayoutDir
    if (-not (Read-YesNo ("Move them into $script:OldLayoutDir\ (the new build refuses to start beside them)?") $true)) {
        Write-Warn 'Left in place. The new Multivoid will refuse to start until they are removed.'
        return @()
    }
    if (-not (Test-Path -LiteralPath $aside)) { New-Item -ItemType Directory -Path $aside -Force | Out-Null }
    foreach ($v in $victims) {
        $dest = Join-Path $aside (Split-Path -Leaf $v)
        if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Force }
        Move-Item -LiteralPath $v -Destination $dest -Force
        $moved.Add([pscustomobject]@{ Original = (Split-Path -Leaf $v); MovedTo = ($script:OldLayoutDir + '\' + (Split-Path -Leaf $v)) })
        Write-Ok ("Moved aside: " + (Split-Path -Leaf $v))
    }
    return $moved.ToArray()
}

function Copy-PayloadFile($Layout, $Entry) {
    # Returns 'new', 'updated' or 'ok' (already identical). Verifies the copy by hash.
    $dest = Resolve-Dest $Layout $Entry.Base $Entry.Rel
    $srcHash = Get-Sha256 $Entry.Src
    $status = 'new'
    if (Test-Path -LiteralPath $dest -PathType Leaf) {
        if ((Get-Sha256 $dest) -eq $srcHash) { return [pscustomobject]@{ Status = 'ok'; Sha256 = $srcHash; Dest = $dest } }
        $status = 'updated'
    }
    $dir = Split-Path -Parent $dest
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    Copy-Item -LiteralPath $Entry.Src -Destination $dest -Force
    if ((Get-Sha256 $dest) -ne $srcHash) { Stop-Installer "Copy verification failed for $($Entry.Rel) (the file on disk does not match the payload)." }
    return [pscustomobject]@{ Status = $status; Sha256 = $srcHash; Dest = $dest }
}

function Invoke-Install([bool]$Repair) {
    $verb = if ($Repair) { 'Repair' } else { 'Install' }
    $payload = Get-Payload
    Write-Step 'Locating the game'
    $win64 = Select-GameFolder
    $layout = Get-Layout $win64
    Write-Ok "Game: $win64"
    Confirm-GameVersion $layout
    Assert-GameClosed $layout

    $old = Read-Manifest $layout
    if ($Repair -and $null -eq $old -and -not (Test-Path -LiteralPath $layout.ModDir -PathType Container)) {
        Stop-Installer 'Nothing to repair: Multivoid is not installed here. Choose Install instead.'
    }
    if ($Repair -and $null -eq $old) { Write-Warn 'No install manifest found; repairing as a fresh install (a new manifest is written).' }

    Write-Step "$verb : checking for conflicts"
    $oldLayout = @(Move-OldLayoutLeftovers $layout)
    $plan = Resolve-LoaderPlan $layout $payload $old $Repair

    # --- from here the disk changes ---------------------------------------------------------------------------
    Write-Step "$verb : writing files"
    $backups = New-Object System.Collections.Generic.List[object]
    foreach ($b in (Get-ManifestArray $old 'backups')) {       # backups from earlier runs stay recorded while they exist
        if (Test-Path -LiteralPath (Join-Path $layout.Win64 $b.backup) -PathType Leaf) {
            $backups.Add([pscustomobject]@{ original = $b.original; backup = $b.backup; sha256 = $b.sha256 })
        }
    }
    foreach ($b in $plan.Backups) {
        $src = Join-Path $layout.Win64 $b.Original
        $dst = New-BackupName $src
        Move-Item -LiteralPath $src -Destination $dst -Force
        $rel = Get-RelativePath $layout.Win64 $dst
        $backups.Add([pscustomobject]@{ original = $b.Original; backup = $rel; sha256 = $b.Sha256 })
        Write-Ok "Backed up $($b.Original) -> $rel"
    }

    $entries = New-Object System.Collections.Generic.List[object]
    foreach ($e in $payload.Files) { $entries.Add($e) }
    if ($plan.Install) { $entries.Add($payload.Loader) }

    $manifestFiles = New-Object System.Collections.Generic.List[object]
    $counts = @{ new = 0; updated = 0; ok = 0 }
    foreach ($e in $entries) {
        $r = Copy-PayloadFile $layout $e
        $counts[$r.Status]++
        $manifestFiles.Add([ordered]@{ base = $e.Base; path = $e.Rel; sha256 = $r.Sha256; size = (Get-Item -LiteralPath $r.Dest).Length })
        switch ($r.Status) {
            'new'     { Write-Ok ("added     " + $(if ($e.Base -eq 'pak') { 'LogicMods\multivoid\' } else { '' }) + $e.Rel) }
            'updated' { Write-Ok ("restored  " + $e.Rel + $(if ($Repair) { ' (was missing or changed)' } else { ' (replaced)' })) }
            default   { if ($Repair) { Write-Info ("unchanged " + $e.Rel) } }
        }
    }

    # Files an earlier install owned that this payload no longer has.
    $keep = @{}
    foreach ($m in $manifestFiles) { $keep[($m.base + '|' + $m.path).ToLowerInvariant()] = $true }
    foreach ($f in (Get-ManifestArray $old 'files')) {
        if ($keep.ContainsKey(([string]$f.base + '|' + [string]$f.path).ToLowerInvariant())) { continue }
        $dest = Resolve-Dest $layout $f.base $f.path
        if (Test-Path -LiteralPath $dest -PathType Leaf) {
            if ($f.path -ieq 'dwmapi.dll' -and -not (Test-IsMultivoidLoader $dest)) { continue }
            Remove-Item -LiteralPath $dest -Force
            Write-Ok "removed   $($f.path) (no longer part of the payload)"
        }
    }

    $allOldMoved = @($oldLayout) + @(Get-ManifestArray $old 'oldLayoutMoved')
    Write-Manifest $layout $manifestFiles.ToArray() $backups.ToArray() $plan.Install @($allOldMoved)
    Write-Ok "Manifest written: Mods\Multivoid\$script:ManifestName"

    Write-Step "$verb done"
    Write-Host ("  files: {0} added, {1} replaced, {2} already correct" -f $counts['new'], $counts['updated'], $counts['ok'])
    if ($plan.Install) {
        Write-Ok 'The Multivoid loader (dwmapi.dll) is in place. Start the game; the main menu shows a Multiplayer entry.'
    } else {
        Write-Warn 'The mod is installed, but it relies on UE4SS to start it. Start the game through your UE4SS setup.'
    }
    Write-Host '  Loader log, after a run: Mods\Multivoid\loader.log   Mod log: multivoid.log (beside the exe)'
}

# ---------------------------------------------------------------------------------------------------------------
# Uninstall
# ---------------------------------------------------------------------------------------------------------------
function Invoke-Uninstall {
    Write-Step 'Locating the game'
    $win64 = Select-GameFolder
    $layout = Get-Layout $win64
    Write-Ok "Game: $win64"
    Assert-GameClosed $layout

    $m = Read-Manifest $layout
    if ($null -eq $m) {
        Stop-Installer ("There is no Mods\Multivoid\$script:ManifestName here, so the installer does not know what it put in.`n" +
                        "Nothing was removed. If you installed by hand, delete the Mods\Multivoid folder and dwmapi.dll yourself;`n" +
                        "or run Repair first, which writes a manifest, then Uninstall.")
    }

    Write-Step 'Uninstall : removing what the installer added'
    $removed = 0
    foreach ($f in (Get-ManifestArray $m 'files')) {
        $dest = Resolve-Dest $layout $f.base $f.path
        if (-not (Test-Path -LiteralPath $dest -PathType Leaf)) { continue }
        if ($f.path -ieq 'dwmapi.dll' -and -not (Test-IsMultivoidLoader $dest)) {
            Write-Warn 'dwmapi.dll is not Multivoid''s any more (something replaced it); leaving it alone.'
            continue
        }
        Remove-Item -LiteralPath $dest -Force
        $removed++
        Write-Ok "removed   $($f.path)"
    }
    if ($removed -eq 0) { Write-Info 'None of the installed files were present.' }

    foreach ($b in (Get-ManifestArray $m 'backups')) {
        $bak = Join-Path $layout.Win64 $b.backup
        $orig = Join-Path $layout.Win64 $b.original
        if (-not (Test-Path -LiteralPath $bak -PathType Leaf)) { Write-Warn "backup $($b.backup) is gone; cannot restore $($b.original)."; continue }
        if (Test-Path -LiteralPath $orig) {
            Write-Warn "$($b.original) exists again, so the backup $($b.backup) is left where it is."
            continue
        }
        Move-Item -LiteralPath $bak -Destination $orig -Force
        Write-Ok "restored  $($b.original)"
    }
    foreach ($o in (Get-ManifestArray $m 'oldLayoutMoved')) {
        Write-Info "Old-layout file $($o.Original) stays in $script:OldLayoutDir\ (delete that folder when you like)."
    }

    # What the installer did not put there: logs, settings, saves, identity.
    $modDir = $layout.ModDir
    $left = @()
    if (Test-Path -LiteralPath $modDir -PathType Container) { $left = @(Get-ChildItem -LiteralPath $modDir -Recurse -File -Force) }
    $manifestItself = $left | Where-Object { $_.FullName -ieq $layout.Manifest }
    $left = @($left | Where-Object { $_.FullName -ine $layout.Manifest })
    $logFiles = @($left | Where-Object { $_.Name -like '*.log' })
    $dataFiles = @($left | Where-Object { $_.Name -notlike '*.log' })
    $diag = New-Object System.Collections.Generic.List[string]
    foreach ($n in @('multivoid.log', 'multivoid.prev.log', 'multivoid-loaded.txt', 'multivoid-compat-report.txt', 'multivoid_reports')) {
        $p = Join-Path $layout.Win64 $n
        if (Test-Path -LiteralPath $p) { $diag.Add($p) }
    }
    $locale = Join-Path $layout.Win64 'multivoid_locale'

    if ($logFiles.Count -gt 0 -or $diag.Count -gt 0) {
        Write-Host ''
        Write-Host ("Multivoid left {0} log file(s) in its folder and {1} diagnostic item(s) beside the exe (logs, saved bug reports)." -f $logFiles.Count, $diag.Count)
        $doLogs = if ($script:Interactive -and -not $RemoveLogs) { Read-YesNo 'Delete them?' $true } else { [bool]$RemoveLogs }
        if ($doLogs) {
            foreach ($f in $logFiles) { Remove-Item -LiteralPath $f.FullName -Force }
            foreach ($d in $diag) { Remove-Item -LiteralPath $d -Recurse -Force }
            Write-Ok 'Logs and reports deleted.'
        } else { Write-Info 'Logs and reports kept.' }
    }
    $hasLocale = Test-Path -LiteralPath $locale
    if ($dataFiles.Count -gt 0 -or $hasLocale) {
        Write-Host ''
        Write-Host ("Your own Multivoid data is still there ({0} file(s) in Mods\Multivoid: settings, saved inventories, identity key{1})." -f $dataFiles.Count, $(if ($hasLocale) { ', translations' } else { '' }))
        Write-Warn 'Deleting the identity key (multivoid_identity.key) orphans the inventory hosts stored for you.'
        $doData = if ($script:Interactive -and -not $RemoveUserData) { Read-YesNo 'Delete your Multivoid settings and data too?' $false } else { [bool]$RemoveUserData }
        if ($doData) {
            foreach ($f in $dataFiles) { Remove-Item -LiteralPath $f.FullName -Force }
            if ($hasLocale) { Remove-Item -LiteralPath $locale -Recurse -Force }
            Write-Ok 'Settings and data deleted.'
        } else { Write-Info 'Settings and data kept (a later reinstall picks them up).' }
    }

    if ($manifestItself -and (Test-Path -LiteralPath $layout.Manifest)) {
        $remaining = @(Get-ChildItem -LiteralPath $modDir -Recurse -File -Force | Where-Object { $_.FullName -ine $layout.Manifest })
        if ($remaining.Count -eq 0) { Remove-Item -LiteralPath $layout.Manifest -Force }
        else { Write-Info 'The install manifest stays because kept data remains in the mod folder.' }
    }
    Remove-EmptyDirs $modDir
    Remove-EmptyDirs $layout.PakDir
    $modsRoot = Join-Path $layout.Win64 'Mods'
    if ((Test-Path -LiteralPath $modsRoot -PathType Container) -and @(Get-ChildItem -LiteralPath $modsRoot -Force).Count -eq 0) {
        Remove-Item -LiteralPath $modsRoot -Force
    }
    Write-Step 'Uninstall done'
    Write-Ok 'The game boots stock again.'
}

# ---------------------------------------------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------------------------------------------
function Show-Banner {
    Write-Host ''
    Write-Host '  Multivoid installer' -ForegroundColor Cyan -NoNewline
    Write-Host ("   for Voices of the Void {0}   (installer {1})" -f $script:GameTarget, $script:InstallerVersion) -ForegroundColor DarkGray
    Write-Host ''
}

function Read-MenuChoice {
    Write-Host '  1) Install'
    Write-Host '  2) Uninstall'
    Write-Host '  3) Repair'
    Write-Host '  Q) Quit'
    while ($true) {
        $a = (Read-Host 'Pick one').Trim().ToLowerInvariant()
        switch ($a) {
            '1' { return 'Install' }
            '2' { return 'Uninstall' }
            '3' { return 'Repair' }
            'q' { return $null }
        }
    }
}

$exitCode = 0
try {
    Show-Banner
    $chosen = $Action
    if (-not $chosen) {
        if (-not $script:Interactive) { Stop-Installer 'Unattended runs need -Action.' }
        $chosen = Read-MenuChoice
    }
    switch ($chosen) {
        'Install'   { Invoke-Install $false }
        'Repair'    { Invoke-Install $true }
        'Uninstall' { Invoke-Uninstall }
        default     { Write-Info 'Nothing done.' }
    }
} catch [System.InvalidOperationException] {
    Write-Host ''
    Write-Fail $_.Exception.Message
    $exitCode = 1
} catch {
    Write-Host ''
    Write-Fail ('Unexpected error: ' + $_.Exception.Message)
    Write-Host ($_.ScriptStackTrace) -ForegroundColor DarkGray
    $exitCode = 2
}
exit $exitCode
