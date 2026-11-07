<#
.SYNOPSIS
    Assembles the friend installer package dist\Multivoid-Installer-<version>\ and zips it.

.DESCRIPTION
    Takes the build outputs (main.dll, the dwmapi.dll loader) from build\votv-coop\Release, the starter skin paks
    from assets\paks (when present), and lays them out as the installer expects:

        Multivoid-Installer-<version>\
            Install-Multivoid.bat / Install-Multivoid.ps1 / install-multivoid.sh (Linux, LF endings)
            README.md / INSTALL-WINDOWS.md / INSTALL-LINUX.md / LICENSE / THIRD-PARTY-NOTICES.md / VERSION.txt
            payload\mod\dlls\main.dll [+ main.dll.sig]   payload\mod\enabled.txt
            payload\loader\dwmapi.dll
            payload\pak\*                                (optional)

    A release-signed main.dll comes from the release package zip: pass it as -PackageZip and its mod\ and pak\
    folders are used instead of main.dll from the build directory (the loader still comes from the build).

.PARAMETER BuildDir
    Where main.dll and dwmapi.dll are. Default build\votv-coop\Release.
.PARAMETER Version
    The game version the installer targets; names the folder and the zip. Default 0.10.0.
.PARAMETER OutDir
    Default dist.
.PARAMETER PackageZip
    Optional: the release package zip (Pelmentor-Multivoid-<version>.zip) to take mod\ and pak\ from.
.PARAMETER PakDir
    Default assets\paks. Used only when -PackageZip is not given.
.PARAMETER MainDll, LoaderDll
    Override the two build outputs.
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build\votv-coop\Release',
    [string]$Version = '0.10.0',
    [string]$OutDir = 'dist',
    [string]$PackageZip = '',
    [string]$PakDir = '',
    [string]$MainDll = '',
    [string]$LoaderDll = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
function Resolve-RepoPath([string]$p) {
    if ([System.IO.Path]::IsPathRooted($p)) { return $p }
    return (Join-Path $repoRoot $p)
}
function Get-Sha256([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

$buildAbs = Resolve-RepoPath $BuildDir
if (-not $MainDll)   { $MainDll   = Join-Path $buildAbs 'main.dll' }
if (-not $LoaderDll) { $LoaderDll = Join-Path $buildAbs 'dwmapi.dll' }
if (-not (Test-Path -LiteralPath $LoaderDll -PathType Leaf)) {
    throw "loader not found: $LoaderDll -- build it: cmake --build build/votv-coop --config Release --target multivoid-loader"
}

$name  = "Multivoid-Installer-$Version"
$outAbs = Resolve-RepoPath $OutDir
$stage = Join-Path $outAbs $name
$zip   = Join-Path $outAbs ($name + '.zip')
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
if (Test-Path -LiteralPath $zip)   { Remove-Item -LiteralPath $zip -Force }
New-Item -ItemType Directory -Path (Join-Path $stage 'payload\mod\dlls') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stage 'payload\loader') -Force | Out-Null

# --- the installer itself ---------------------------------------------------------------------------------------
foreach ($f in 'Install-Multivoid.ps1', 'Install-Multivoid.bat', 'README.md', 'INSTALL-WINDOWS.md', 'INSTALL-LINUX.md') {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $f) -Destination $stage
}
# The Linux installer is copied as raw bytes with CR stripped: a CRLF shebang ("bash\r") would not run on Linux.
$shBytes = [System.IO.File]::ReadAllBytes((Join-Path $PSScriptRoot 'install-multivoid.sh'))
$shLf = New-Object System.Collections.Generic.List[byte] ($shBytes.Length)
foreach ($b in $shBytes) { if ($b -ne 13) { $shLf.Add($b) } }
[System.IO.File]::WriteAllBytes((Join-Path $stage 'install-multivoid.sh'), $shLf.ToArray())
foreach ($legal in 'LICENSE', 'THIRD-PARTY-NOTICES.md') {
    $p = Join-Path $repoRoot $legal
    if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination $stage }
}

# --- the payload ------------------------------------------------------------------------------------------------
$pakCount = 0
$source = ''
if ($PackageZip) {
    $zipAbs = Resolve-RepoPath $PackageZip
    if (-not (Test-Path -LiteralPath $zipAbs -PathType Leaf)) { throw "package zip not found: $zipAbs" }
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('mv-pkg-' + [guid]::NewGuid().ToString('N'))
    try {
        Expand-Archive -LiteralPath $zipAbs -DestinationPath $tmp -Force
        $modSrc = Join-Path $tmp 'mod'
        if (-not (Test-Path -LiteralPath (Join-Path $modSrc 'dlls\main.dll'))) { throw "$zipAbs has no mod\dlls\main.dll" }
        Copy-Item -Path (Join-Path $modSrc '*') -Destination (Join-Path $stage 'payload\mod') -Recurse -Force
        $pakSrc = Join-Path $tmp 'pak'
        if (Test-Path -LiteralPath $pakSrc) {
            New-Item -ItemType Directory -Path (Join-Path $stage 'payload\pak') -Force | Out-Null
            Copy-Item -Path (Join-Path $pakSrc '*') -Destination (Join-Path $stage 'payload\pak') -Recurse -Force
        }
    } finally { if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force } }
    $source = "main.dll from $(Split-Path -Leaf $zipAbs)"
} else {
    if (-not (Test-Path -LiteralPath $MainDll -PathType Leaf)) {
        throw "main.dll not found: $MainDll -- build first: cmake --build build/votv-coop --config Release"
    }
    Copy-Item -LiteralPath $MainDll -Destination (Join-Path $stage 'payload\mod\dlls\main.dll')
    $sig = $MainDll + '.sig'
    if (Test-Path -LiteralPath $sig) { Copy-Item -LiteralPath $sig -Destination (Join-Path $stage 'payload\mod\dlls\main.dll.sig') }
    $pakAbs = if ($PakDir) { Resolve-RepoPath $PakDir } else { Join-Path $repoRoot 'assets\paks' }
    if (Test-Path -LiteralPath $pakAbs -PathType Container) {
        $paks = @(Get-ChildItem -LiteralPath $pakAbs -File | Where-Object { $_.Extension -in '.pak', '.png' })
        if ($paks.Count -gt 0) {
            New-Item -ItemType Directory -Path (Join-Path $stage 'payload\pak') -Force | Out-Null
            foreach ($p in $paks) { Copy-Item -LiteralPath $p.FullName -Destination (Join-Path $stage 'payload\pak') }
        }
    }
    $source = "main.dll from $MainDll"
}
# UE4SS reads the file's presence; the field packages ship the literal "true" inside, so does the release zip.
Set-Content -LiteralPath (Join-Path $stage 'payload\mod\enabled.txt') -Value 'true' -Encoding ascii -NoNewline
Copy-Item -LiteralPath $LoaderDll -Destination (Join-Path $stage 'payload\loader\dwmapi.dll')
if (Test-Path -LiteralPath (Join-Path $stage 'payload\pak')) {
    $pakCount = @(Get-ChildItem -LiteralPath (Join-Path $stage 'payload\pak') -Recurse -File).Count
}

# --- verify what was staged -------------------------------------------------------------------------------------
foreach ($need in 'payload\mod\dlls\main.dll', 'payload\mod\enabled.txt', 'payload\loader\dwmapi.dll',
                  'Install-Multivoid.ps1', 'Install-Multivoid.bat', 'install-multivoid.sh', 'README.md',
                  'INSTALL-WINDOWS.md', 'INSTALL-LINUX.md') {
    if (-not (Test-Path -LiteralPath (Join-Path $stage $need) -PathType Leaf)) { throw "staged tree is missing $need" }
}
$mainSha = Get-Sha256 (Join-Path $stage 'payload\mod\dlls\main.dll')
$loaderSha = Get-Sha256 (Join-Path $stage 'payload\loader\dwmapi.dll')
$hasSig = Test-Path -LiteralPath (Join-Path $stage 'payload\mod\dlls\main.dll.sig')

$proto = ''
$protoH = Join-Path $repoRoot 'src\votv-coop\include\coop\net\protocol.h'
if (Test-Path -LiteralPath $protoH) {
    $m = [regex]::Match((Get-Content -LiteralPath $protoH -Raw), 'kProtocolVersion = (\d+)')
    if ($m.Success) { $proto = $m.Groups[1].Value }
}
$info = @(
    "Multivoid installer package for Voices of the Void $Version",
    "build (protocol) : $(if ($proto) { 'b' + $proto } else { 'unknown' })",
    "main.dll sha256  : $mainSha  $(if ($hasSig) { '(release-signed)' } else { '(unsigned: the game will call this build unofficial)' })",
    "dwmapi.dll sha256: $loaderSha",
    "paks             : $pakCount file(s)",
    "assembled        : $((Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm')) UTC"
)
Set-Content -LiteralPath (Join-Path $stage 'VERSION.txt') -Value $info -Encoding ascii

# --- zip (forward-slash entry names, one top folder) ------------------------------------------------------------
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$stageFull = (Get-Item -LiteralPath $stage).FullName
$fs = [System.IO.File]::Open($zip, [System.IO.FileMode]::CreateNew)
try {
    $archive = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in @(Get-ChildItem -LiteralPath $stageFull -Recurse -File)) {
            $entry = $name + '/' + (($f.FullName.Substring($stageFull.Length).TrimStart('\')) -replace '\\', '/')
            $e = [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $entry, [System.IO.Compression.CompressionLevel]::Optimal)
            if ($f.Extension -eq '.sh') {
                # Best effort: record mode 0755 so `unzip` on Linux keeps the executable bit (README says chmod +x anyway).
                try { $e.ExternalAttributes = [System.BitConverter]::ToInt32([System.BitConverter]::GetBytes([uint32]0x81ED0000), 0) } catch { }
            }
        }
    } finally { $archive.Dispose() }
} finally { $fs.Dispose() }

# --- verify the written zip -------------------------------------------------------------------------------------
$read = [System.IO.Compression.ZipFile]::OpenRead($zip)
try {
    $names = @($read.Entries | ForEach-Object { $_.FullName })
    foreach ($need in "$name/Install-Multivoid.ps1", "$name/Install-Multivoid.bat", "$name/install-multivoid.sh",
                      "$name/INSTALL-WINDOWS.md", "$name/INSTALL-LINUX.md", "$name/payload/mod/dlls/main.dll",
                      "$name/payload/mod/enabled.txt", "$name/payload/loader/dwmapi.dll") {
        if ($names -notcontains $need) { throw "the written zip is missing $need" }
    }
} finally { $read.Dispose() }

Write-Host ''
Write-Host "PACKAGE OK  $name" -ForegroundColor Green
Write-Host "  folder : $stage"
Write-Host "  zip    : $zip"
Write-Host "  sha256 : $(Get-Sha256 $zip)"
Write-Host "  payload: $source; dwmapi.dll from $LoaderDll; $pakCount pak file(s)$(if (-not $hasSig) { '; main.dll UNSIGNED' })"
