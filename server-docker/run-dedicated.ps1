<#
server-docker/run-dedicated.ps1 -- run a Multivoid dedicated server on Windows, in this console.

The game runs headless (-nullrhi); the mod attaches to this console, so its log prints here and
the lines typed here are its commands (stop, save, say <msg>, players, kick <name>, help).
Ctrl+C also stops it, with a save. docs/dedicated-server.md is the design.

  .\run-dedicated.ps1 -GameDir "D:\Games\VotV" -SaveSlot myworld -Port 47621 -Name "Our world"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $GameDir,
    [string] $SaveSlot = "multivoid_server",
    [int]    $Port = 0,
    [string] $Name = "Multivoid Server",
    [string] $Password = "",
    [ValidateRange(2, 10)] [int] $MaxPlayers = 10,
    [ValidateSet("0", "1")] [string] $ListPublic = "1",
    [ValidateSet("direct", "brokered")] [string] $Reach = "direct",
    [int]    $AutosaveMinutes = 10,
    [switch] $Windowed,          # a real renderer instead of -nullrhi, for a first-run check
    [string] $ExtraArgs = ""
)

$ErrorActionPreference = "Stop"
$exe = Get-ChildItem -Path $GameDir -Recurse -Depth 6 -Filter "VotV-Win64-Shipping.exe" -File |
    Select-Object -First 1
if (-not $exe) { throw "No VotV-Win64-Shipping.exe under '$GameDir'." }
$mod = Get-ChildItem -Path $exe.DirectoryName -Recurse -Depth 4 -Filter "main.dll" -File |
    Where-Object { $_.FullName -match '[\\/]Mods[\\/]Multivoid[\\/]dlls[\\/]main\.dll$' } |
    Select-Object -First 1
if (-not $mod) { throw "Multivoid is not installed beside '$($exe.FullName)' (no Mods\Multivoid\dlls\main.dll)." }

# The settings, as the mod reads them; the child inherits this process's environment.
$env:MULTIVOID_SERVER           = "1"
$env:MULTIVOID_SERVER_NAME      = $Name
$env:MULTIVOID_PASSWORD         = $Password
$env:MULTIVOID_MAX_PLAYERS      = "$MaxPlayers"
$env:MULTIVOID_SAVE_SLOT        = $SaveSlot
$env:MULTIVOID_LIST_PUBLIC      = $ListPublic
$env:MULTIVOID_REACH            = $Reach
$env:MULTIVOID_AUTOSAVE_MINUTES = "$AutosaveMinutes"
$env:MULTIVOID_STDOUT           = "1"
if ($Port -gt 0) { $env:MULTIVOID_PORT = "$Port" }

$gameArgs = @("-nosound", "-unattended", "-nosplash", "-NoVerifyGC", "-multivoid-server")
if (-not $Windowed) { $gameArgs += "-nullrhi" }
if ($ExtraArgs) { $gameArgs += $ExtraArgs }

Write-Host "run-dedicated: $($exe.FullName) $($gameArgs -join ' ')"
Write-Host "run-dedicated: the log is also in $(Join-Path $exe.DirectoryName 'multivoid.log')"
# -NoNewWindow keeps the game on this console: it attaches to it for its stdout and stdin.
$p = Start-Process -FilePath $exe.FullName -ArgumentList $gameArgs -WorkingDirectory $exe.DirectoryName `
    -NoNewWindow -PassThru
$p.WaitForExit()
Write-Host "run-dedicated: the server exited (code $($p.ExitCode))"
exit $p.ExitCode
