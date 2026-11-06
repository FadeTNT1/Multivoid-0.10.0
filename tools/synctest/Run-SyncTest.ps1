#requires -Version 7.0
[CmdletBinding()]
param(
    [switch]$ReuseBuild,
    [string[]]$Events,
    [ValidateRange(8,120)][int]$ObserveSeconds = 12,
    [ValidateRange(60,600)][int]$JoinSeconds = 300,
    [ValidateRange(1024,65535)][int]$Port = 28771
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$source = 'C:\Users\andre\Desktop\VOTV 0.10.0\a0.10.0_0009\WindowsNoEditor'
$rig = 'C:\Users\andre\Desktop\VOTV 0.10.0\rig'
$build = Join-Path $repo 'build/synctest'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$report = Join-Path $repo "local/tests/synctest/$stamp"
$lockPath = 'C:\Users\andre\AppData\Local\Temp\multivoid-synctest.lock'
$realSaves = 'C:\Users\andre\AppData\Local\VotV\Saved\SaveGames'
$lock = $null; $guard = $null
$peers = @{}; $results = [Collections.Generic.List[object]]::new()
$pairNumber = 0
. "$PSScriptRoot/Verdict.ps1"

function Read-Log($path) {
    if (!(Test-Path -LiteralPath $path)) { return '' }
    $stream = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite,Delete')
    try { $reader = [IO.StreamReader]::new($stream); return $reader.ReadToEnd() }
    finally { $stream.Dispose() }
}
function Save-Manifest {
    if (Test-Path -LiteralPath $realSaves) {
        Get-ChildItem -LiteralPath $realSaves -File -Recurse | Sort-Object FullName | ForEach-Object {
            "$([IO.Path]::GetRelativePath($realSaves,$_.FullName))|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)"
        }
    }
}
function Check-Peers {
    if ($guard.Failure) { throw $guard.Failure }
    "$([DateTime]::UtcNow.ToString('o')) $([SyncTestProcesses]::CursorStatus())" | Add-Content -LiteralPath "$report/cursor.log"
    foreach ($peer in $peers.Values) {
        $process = Get-Process -Id $peer.Pid -ErrorAction SilentlyContinue
        if (!$process) { throw "Rig $($peer.Role) exited (PID $($peer.Pid))" }
        "$([DateTime]::UtcNow.ToString('o')),$($peer.Role),$($peer.Pid),$($process.WorkingSet64),$($process.CPU)" |
            Add-Content -LiteralPath "$report/resources.csv"
    }
}
function Write-Command($role, $text) {
    for ($attempt=0; $attempt -lt 40; $attempt++) {
        try { [IO.File]::WriteAllText("$report/$role.command", $text + "`n", [Text.UTF8Encoding]::new($false)); return }
        catch [IO.IOException] { Check-Peers; Start-Sleep -Milliseconds 50 }
    }
    throw "Cannot update $role command after two seconds"
}
function Wait-Log($path, $pattern, $seconds) {
    $deadline = [DateTime]::UtcNow.AddSeconds($seconds)
    do {
        Check-Peers
        if ((Read-Log $path) -match $pattern) { return }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timeout waiting for '$pattern' in $path"
}
function Prepare-Copy($role) {
    $dest = Join-Path $rig $role
    if (Test-Path -LiteralPath $dest) {
        if (!(Test-Path -LiteralPath "$dest/.synctest-owner")) { throw "Refusing unowned rig folder: $dest" }
        if ((Get-Item -LiteralPath $dest).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Rig folder is a reparse point: $dest" }
        if (Get-ChildItem -LiteralPath $dest -Directory -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) { throw "Rig copy contains a directory reparse point: $dest" }
    } else { New-Item -ItemType Directory -Path $dest | Out-Null }
    'Run-SyncTest.ps1' | Set-Content -LiteralPath "$dest/.synctest-owner"
    foreach ($file in Get-ChildItem -LiteralPath $source -File -Recurse) {
        $rel = [IO.Path]::GetRelativePath($source, $file.FullName)
        if ($rel -match '(^|[\\/])(Mods|Saved|ue4ss)([\\/]|$)' -or
            $file.Name -match '^(dwmapi\.dll|UE4SS\.dll|multivoid.*|loader\.log)$') { continue }
        if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Source reparse point: $($file.FullName)" }
        $target = Join-Path $dest $rel
        if (Test-Path -LiteralPath $target) { continue }
        New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
        if ($file.Length -gt 16MB -and $file.Extension -in '.pak','.ucas','.utoc') {
            New-Item -ItemType HardLink -Path $target -Target $file.FullName | Out-Null
        } else { Copy-Item -LiteralPath $file.FullName -Destination $target }
    }
    $bin = Join-Path $dest 'VotV/Binaries/Win64'
    $mod = Join-Path $bin 'Mods/Multivoid'
    New-Item -ItemType Directory -Path "$mod/dlls" -Force | Out-Null
    # Remove destination files first: never overwrite an inode inherited from a previous hardlink.
    foreach ($pair in @(@("$build/Release/main.dll", "$mod/dlls/main.dll"), @("$build/Release/dwmapi.dll", "$bin/dwmapi.dll"))) {
        if (Test-Path -LiteralPath $pair[1]) { Remove-Item -LiteralPath $pair[1] }
        Copy-Item -LiteralPath $pair[0] -Destination $pair[1]
    }
    '1' | Set-Content -LiteralPath "$mod/enabled.txt"
    @"
[net]
role=$role
topology=lan
peer=127.0.0.1
port=$Port
masters=Rig=http://127.0.0.1:9
ice=all
[dev]
enabled=1
save=synctest_world
"@ | Set-Content -LiteralPath "$mod/multivoid.ini" -Encoding utf8NoBOM
    return $bin
}
function Start-Peer($role, $bin) {
    $private = Join-Path $rig "synctest_$role"
    if ((Test-Path -LiteralPath $private) -and ((Get-Item -LiteralPath $private).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Private user root is a reparse point: $private" }
    if ((Test-Path -LiteralPath $private) -and (Get-ChildItem -LiteralPath $private -Directory -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint })) { throw "Private user data contains a directory reparse point: $private" }
    New-Item -ItemType Directory -Path $private -Force | Out-Null
    $inputSettings = @'
[/Script/Engine.InputSettings]
bCaptureMouseOnLaunch=False
DefaultViewportMouseCaptureMode=NoCapture
DefaultViewportMouseLockMode=DoNotLock
'@
    foreach ($config in @("$private/VotV/Saved/Config/WindowsNoEditor/Input.ini", "$(Split-Path (Split-Path $bin))/Config/DefaultInput.ini")) {
        New-Item -ItemType Directory -Path (Split-Path $config) -Force | Out-Null
        $inputSettings | Set-Content -LiteralPath $config -Encoding utf8NoBOM
    }
    $envs = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) {
        if ($entry.Key -notmatch '^VOTVCOOP_|^SHIMLOADER_') { $envs[$entry.Key] = $entry.Value }
    }
    $envs['LOCALAPPDATA'] = $private
    $envs['VOTVCOOP_SCENARIO'] = 'synctest'
    $envs['MULTIVOID_HARNESS_NO_INPUT_CAPTURE'] = '1'
    $envs['VOTVCOOP_NET_ROLE'] = $role
    $envs['VOTVCOOP_NET_TOPOLOGY'] = 'lan'
    $envs['VOTVCOOP_NET_PEER'] = '127.0.0.1'
    $envs['VOTVCOOP_NET_PORT'] = "$Port"
    $envs['VOTVCOOP_NET_MASTERS'] = 'Rig=http://127.0.0.1:9'
    $envs['VOTVCOOP_NET_SIGNALING'] = '127.0.0.1:9'
    $envs['VOTVCOOP_NET_NICK'] = "synctest_$role"
    $envs['VOTVCOOP_SAVE'] = 'synctest_world'
    $envs['VOTVCOOP_SYNCTEST_COMMAND'] = "$report/$role.command"
    $commands = 't.MaxFPS 24,sg.ResolutionQuality 25,sg.ViewDistanceQuality 0,sg.AntiAliasingQuality 0,sg.ShadowQuality 0,sg.PostProcessQuality 0,sg.TextureQuality 0,sg.EffectsQuality 0,sg.FoliageQuality 0,r.VSync 0,t.IdleWhenNotForeground 0'
    $args = "-windowed -ForceRes -ResX=640 -ResY=360 -WinX=-32000 -WinY=-32000 -nosplash -nosound -RenderOffscreen -SaveToUserDir -UserDir=`"$private/VotV/`" -ExecCmds=`"$commands`""
    $log = Join-Path $bin 'multivoid.log'
    if (Test-Path -LiteralPath $log) { Move-Item -LiteralPath $log -Destination "$report/$role.previous-$pairNumber.log" }
    $peerPid = $guard.Start((Join-Path $bin 'VotV-Win64-Shipping.exe'), $args, $envs)
    $peers[$role] = [pscustomobject]@{ Role=$role; Pid=$peerPid; Log=$log; Bin=$bin }
    "$role PID $peerPid (hidden, BelowNormal, <=4 cores, 24 fps)" | Write-Host
}
function Stop-Pair {
    if ($script:guard) { $script:guard.Dispose(); $script:guard = $null }
    if (!$peers.Count) { return }
    $archive = Join-Path $report ('pair-{0:D2}' -f $pairNumber)
    New-Item -ItemType Directory -Path $archive -Force | Out-Null
    foreach ($peer in $peers.Values) {
        if (Test-Path -LiteralPath $peer.Log) {
            Copy-Item -LiteralPath $peer.Log -Destination "$archive/$($peer.Role).log"
            Copy-Item -LiteralPath $peer.Log -Destination "$report/$($peer.Role).log" -Force
            Select-String -LiteralPath $peer.Log -Pattern 'atv: a parked mirror would not stay' | ForEach-Object {
                "$($peer.Role): $($_.Line)"
            } | Add-Content -LiteralPath "$report/desync-warnings.log"
        }
        $loader = Join-Path $peer.Bin 'Mods/Multivoid/loader.log'
        if (Test-Path -LiteralPath $loader) { Copy-Item -LiteralPath $loader -Destination "$archive/$($peer.Role)-loader.log" }
    }
    $script:peers = @{}
}
function Start-Pair {
    $script:pairNumber++
    foreach ($role in 'host','client') {
        $command = "$report/$role.command"
        if (Test-Path -LiteralPath $command) { Remove-Item -LiteralPath $command }
    }
    $script:guard = [SyncTestProcesses]::new()
    Start-Peer host $hostBin
    Wait-Log $peers.host.Log 'synctest: INPUT GUARD PASS' 30
    Wait-Log $peers.host.Log 'synctest: listener bound to 127\.0\.0\.1:' $JoinSeconds
    Start-Peer client $clientBin
    Wait-Log $peers.client.Log 'synctest: INPUT GUARD PASS' 30
    Wait-Log $peers.host.Log 'synctest: DRIVER READY role=host' $JoinSeconds
    Wait-Log $peers.client.Log 'synctest: LOCAL ONLY role=client' $JoinSeconds
    foreach ($peer in $peers.Values) {
        $boot = Read-Log $peer.Log
        if ($boot -notmatch 'synctest: SAVE ISOLATION PASS') { throw 'Save isolation not proven' }
        $boot -split "`r?`n" | Where-Object { $_ -match 'posted task.*FAULT|\[(ERROR|E)\s*\]' } |
            Set-Content -LiteralPath "$report/$($peer.Role)-startup-faults-$pairNumber.txt"
        $boot -split "`r?`n" | Where-Object { $_ -match 'net: peer slot.*CONNECTED|ADMITTED pending|rig: READY peer-world-ready|synctest: (INPUT GUARD|SAVE ISOLATION|LOCAL ONLY|listener bound)' } |
            Add-Content -LiteralPath "$report/connection-proof.txt"
    }
    # Join replay is asynchronous; keep its faults separate from the first event's observation.
    $settle = [DateTime]::UtcNow.AddSeconds(30)
    do { Check-Peers; Start-Sleep -Milliseconds 500 } while ([DateTime]::UtcNow -lt $settle)
    foreach ($peer in $peers.Values) {
        (Read-Log $peer.Log) -split "`r?`n" | Where-Object { $_ -match 'posted task.*FAULT|\[(ERROR|E)\s*\]' } |
            Set-Content -LiteralPath "$report/$($peer.Role)-startup-faults-$pairNumber.txt"
    }
}
function Write-Report {
    $results | Export-Csv -LiteralPath "$report/events.csv" -NoTypeInformation
    $results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$report/events.json"
    $lines = @('# Local sync test', '', "Source commit: $(git -C $repo rev-parse HEAD)",
        "Date: $stamp; port: $Port; observe: $ObserveSeconds seconds per case.",
        'One cumulative fresh world; PASS proves the stated log assertion, not all visuals, delayed effects, or late joins.',
        'FAIL kinds: Desync = observed state mismatch; PolicyGap = explicitly missing/host-local lane; Unproven = insufficient effect evidence; Fault/Blocked = infrastructure or runtime failure.',
        'Audio is disabled; sound dispatch is tested but audibility is not.', '', '| Event | Result | Kind | Evidence |', '|---|---|---|---|')
    $startup = @(Get-ChildItem -LiteralPath $report -Filter '*-startup-faults-*.txt' | ForEach-Object { Get-Content -LiteralPath $_.FullName } | Where-Object { $_ })
    if ($startup.Count) { $lines = $lines[0..6] + @("Startup faults: $($startup.Count); see the peer startup-fault files and full logs.",'') + $lines[7..($lines.Count-1)] }
    if (Test-Path -LiteralPath "$report/desync-warnings.log") { $lines += @('', 'Additional pose warnings are retained in desync-warnings.log; a cumulative run does not establish which event caused them.') }
    foreach ($row in $results) { $lines += "| $($row.Name) | $($row.Status) | $($row.Kind) | $(($row.Evidence -replace '\|','/' -replace '[\r\n]+',' ')) |" }
    $lines | Set-Content -LiteralPath "$report/report.md"
}

New-Item -ItemType Directory -Path $report -Force | Out-Null
$table = Get-Content -LiteralPath "$repo/src/votv-coop/src/coop/dev/event_trigger.cpp" -Raw
$cases = @([regex]::Matches($table, '\{\s*"([^"]+)"\s*,\s*D::(\w+)\s*,\s*C::(\w+)') | ForEach-Object {
    [pscustomobject]@{ Name=$_.Groups[1].Value; Dispatch=$_.Groups[2].Value; Category=$_.Groups[3].Value }
})
$cases += @('snow ON','snow OFF','lightning','sky.eye ON','sky.eye OFF','sky.blackhole ON','sky.blackhole OFF','sky.rotation+moon','jellyfish','wind','fireflies','fleshRain','gifts','alarm ON','alarm OFF') | ForEach-Object {
    [pscustomobject]@{ Name=$_; Dispatch='Extra'; Category='Weather/Sky' }
}
if ($Events) {
    foreach ($name in $Events) { if ($name -notin $cases.Name) { throw "Unknown event: $name" } }
    $cases = @($cases | Where-Object Name -In $Events)
}
$cases | Export-Csv -LiteralPath "$report/catalog.csv" -NoTypeInformation
try {
    while (!$lock) {
        try { $lock = [IO.FileStream]::new($lockPath, 'CreateNew', 'ReadWrite', 'None', 4096, [IO.FileOptions]::DeleteOnClose) }
        catch [IO.IOException] { Write-Host 'Waiting for multivoid-synctest.lock'; Start-Sleep -Seconds 5 }
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes("PID=$PID repo=$repo report=$report")
    $lock.Write($bytes, 0, $bytes.Length); $lock.Flush()
    if (Get-CimInstance Win32_Process -Filter "Name='VotV-Win64-Shipping.exe'" | Where-Object { $_.ExecutablePath -like "$rig\*" }) {
        throw 'An existing rig game is running; refusing a second pair'
    }
    & "$PSScriptRoot/Test-Verdict.ps1"
    if (!$ReuseBuild) {
        & cmake -S "$repo/src/votv-coop" -B $build -G 'Visual Studio 17 2022' -A x64 '-DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake' '-DVCPKG_TARGET_TRIPLET=x64-windows-static' '-DVCPKG_MANIFEST_MODE=ON' *> "$report/configure.log"
        if ($LASTEXITCODE) { throw "Configure failed: $report/configure.log" }
        & cmake --build $build --config Release --target votv-coop multivoid-loader --parallel 2 -- /p:CL_MPCount=2 *> "$report/build.log"
        if ($LASTEXITCODE) { throw "Build failed: $report/build.log" }
    }
    foreach ($dll in 'main.dll','dwmapi.dll') { if (!(Test-Path -LiteralPath "$build/Release/$dll")) { throw "Missing own build: $dll" } }
    [pscustomobject]@{
        Commit=(git -C $repo rev-parse HEAD); Branch=(git -C $repo branch --show-current)
        MainDllSHA256=(Get-FileHash "$build/Release/main.dll").Hash
        LoaderSHA256=(Get-FileHash "$build/Release/dwmapi.dll").Hash
    } | ConvertTo-Json | Set-Content -LiteralPath "$report/build-identity.json"
    if (Test-Path -LiteralPath $realSaves) {
        @(Save-Manifest) | Set-Content -LiteralPath "$report/original-saves-before.txt"
        $backup = "$realSaves.synctest-backup-$stamp"
        & robocopy $realSaves $backup /E /COPY:DAT /DCOPY:DAT /XJ /R:0 /W:0 /NFL /NDL /NJH /NJS /NP *> "$report/backup.log"
        if ($LASTEXITCODE -ge 8) { throw "SaveGames backup failed: $report/backup.log" }
        $backup | Set-Content -LiteralPath "$report/save-backup.txt"
    }
    if ((Test-Path -LiteralPath $rig) -and ((Get-Item -LiteralPath $rig).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Rig root is a reparse point: $rig" }
    $hostBin = Prepare-Copy host; $clientBin = Prepare-Copy client
    if (!('SyncTestProcesses' -as [type])) { Add-Type -Path "$PSScriptRoot/ProcessGuard.cs" }
    'utc,role,pid,working_set_bytes,cpu_seconds' | Set-Content -LiteralPath "$report/resources.csv"
    Start-Pair
    $id = 0
    foreach ($case in $cases) {
        $id++; $tag = '{0:D3}' -f $id
        if (!$guard) { Start-Pair }
        try {
            $hBefore = (Read-Log $peers.host.Log).Length; $cBefore = (Read-Log $peers.client.Log).Length
            Write-Command client "$tag|$($case.Name)"
            Wait-Log $peers.client.Log "synctest: BEGIN id=$tag " 15
            Write-Command host "$tag|$($case.Name)"
            # earthTp's sequence runs two minutes; boarwar's first ariral shooter is minted 60 s in.
            $caseSeconds = switch ($case.Name) {
                'earthTp' { [Math]::Max(120,$ObserveSeconds) }
                'boarwar' { [Math]::Max(75,$ObserveSeconds) }
                default { $ObserveSeconds }
            }
            $deadline = [DateTime]::UtcNow.AddSeconds($caseSeconds)
            do { Check-Peers; Start-Sleep -Milliseconds 500 } while ([DateTime]::UtcNow -lt $deadline)
            $h = (Read-Log $peers.host.Log).Substring($hBefore)
            # Each peer buffers logs for up to a second; give the receiver time to flush late births.
            $flushDeadline = [DateTime]::UtcNow.AddSeconds(2)
            do { Check-Peers; Start-Sleep -Milliseconds 500 } while ([DateTime]::UtcNow -lt $flushDeadline)
            $c = (Read-Log $peers.client.Log).Substring($cBefore)
            $h | Set-Content -LiteralPath "$report/$tag-host.log"; $c | Set-Content -LiteralPath "$report/$tag-client.log"
            $verdict = Get-SyncVerdict $case $h $c
        } catch {
            $verdict = [pscustomobject]@{Name=$case.Name; Status='FAIL'; Kind='Fault'; Evidence=$_.Exception.Message}
            Stop-Pair
        }
        $results.Add($verdict); Write-Report
        Write-Host "$tag $($case.Name): $($verdict.Status) [$($verdict.Kind)]"
    }
} catch {
    $failure = $_.Exception.Message
    $failure | Set-Content -LiteralPath "$report/failure.txt"
    foreach ($case in $cases) {
        if ($case.Name -notin @($results | ForEach-Object Name)) {
            $results.Add([pscustomobject]@{Name=$case.Name; Status='FAIL'; Kind='Blocked'; Evidence=$failure})
        }
    }
    Write-Warning $failure
} finally {
    try {
        Stop-Pair
        if (Test-Path -LiteralPath "$report/original-saves-before.txt") {
            $before = @(Get-Content -LiteralPath "$report/original-saves-before.txt")
            $after = @(Save-Manifest)
            $difference = @(Compare-Object $before $after)
            if ($difference.Count) { $difference | Out-String | Set-Content -LiteralPath "$report/original-saves-changed.txt" }
            else { "PASS: $($before.Count) original save files retain their sizes and write times" | Set-Content -LiteralPath "$report/original-saves-check.txt" }
        }
    } finally {
        if ($lock) { $lock.Dispose() }
    }
    Write-Report
    Write-Host "Report: $report/report.md"
}
if (@($results | Where-Object Status -eq 'FAIL').Count) { exit 1 }
