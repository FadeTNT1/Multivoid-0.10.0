$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Verdict.ps1"
function Assert($condition, $message) { if (!$condition) { throw $message } }
$row = [pscustomobject]@{ Name = 'solar'; Dispatch = 'RunEvent' }
$h = "event_fire: host fire #1 runEvent('solar') origin=dev -- broadcasting"
$c = "event_fire: client REPLAY runEvent 'solar'`nevent_fire: runEvent('solar', special='None') dispatched"
Assert ((Get-SyncVerdict $row $h $c).Status -eq 'PASS') 'Valid replay must pass'
Assert ((Get-SyncVerdict $row $h '').Status -eq 'FAIL') 'Missing client evidence must fail'
Assert ((Get-SyncVerdict $row $h ($c + "`n[ERROR] posted task FAULT")).Status -eq 'FAIL') 'Fault must fail'
Assert ((Get-SyncVerdict $row $h ($c + "`n[WARN ] atv: last error 17.2 cm")).Status -eq 'PASS') 'The word error in a warning is not an ERROR log level'
Assert ((Get-SyncVerdict $row $h "event_fire: 'solar' NOT replayed (save-actor spawn (no lane))").Status -eq 'FAIL') 'Receipt without a lane must fail'
Assert ((Get-SyncVerdict $row '' $c).Status -eq 'FAIL') 'Missing host fire must fail'
Assert ((Get-SyncVerdict $row $h "event_fire: client REPLAY runEvent 'solar'").Status -eq 'FAIL') 'Replay intent without dispatch must fail'
$rain = [pscustomobject]@{ Name='rain ON'; Dispatch='Ambient' }
$rainTrigger = "event_trigger: ambient daynightCycle_C::causeRain dispatched ('rain ON')`n"
$on = ((1..3 | ForEach-Object { 'synctest: STATE weather=1 flags=5 fog=0 rain=1.000' }) -join "`n")
$off = $on -replace 'flags=5','flags=4'
Assert ((Get-SyncVerdict $rain ($rainTrigger+$on) $on).Status -eq 'PASS') 'Rain mask must use kIsRaining'
Assert ((Get-SyncVerdict $rain ($rainTrigger+$on) $off).Status -eq 'FAIL') 'Missing rain on client must fail'
Assert ((Get-SyncVerdict $rain ($rainTrigger+$off) $off).Status -eq 'FAIL') 'Equal but unchanged rain state must fail'
$npc = [pscustomobject]@{Name='ventCrawler';Dispatch='RunEvent'}
$nh = "event_fire: host fire #2 runEvent('ventCrawler') origin=dev -- broadcasting`nnpc-sync[host]: tracked EntitySpawn class='ventCrawler_C' elementId=42"
$nc = "npc-sync[client OnSpawn]: materialized mirror eid=42 class='ventCrawler_C' actor=123"
Assert ((Get-SyncVerdict $npc $nh $nc).Status -eq 'PASS') 'Matching class and entity id must pass'
Assert ((Get-SyncVerdict $npc $nh ($nc -replace 'eid=42','eid=43')).Status -eq 'FAIL') 'Different mirror id must fail'
Assert ((Get-SyncVerdict $npc $nh ($nc -replace 'ventCrawler_C','unrelated_C')).Status -eq 'FAIL') 'Unrelated background spawn must fail'
$exh = "event_fire: host fire #2 runEvent('ventCrawler') origin=dev -- broadcasting`nnpc-sync[ex-spawn]: enrolled 'ventCrawler_C' eid=42 at (0,0,0)"
Assert ((Get-SyncVerdict $npc $exh $nc).Status -eq 'PASS') 'Blueprint-internal spawn path must also correlate ids'
$two = $nh + "`nnpc-sync[host]: tracked EntitySpawn class='ventCrawler_C' elementId=43"
Assert ((Get-SyncVerdict $npc $two $nc).Status -eq 'FAIL') 'One missing event actor must fail the event'
$picnic = [pscustomobject]@{Name='picnic';Dispatch='RunEvent'}
$ph = "event_fire: host fire #1 runEvent('picnic') origin=dev -- broadcasting`nHOST broadcasting SPAWN cls='prop_C' key='wa2_picnic'`nHOST broadcasting SPAWN cls='prop_heater_C' key='picnic_heater'"
$pc = "sync::CreateOrAdoptPropMirror: eid=1 bound to actor=1 key='wa2_picnic' cls='prop_C'`nsync::CreateOrAdoptPropMirror: eid=2 bound to actor=2 key='picnic_heater' cls='prop_heater_C'"
Assert ((Get-SyncVerdict $picnic $ph $pc).Status -eq 'PASS') 'Picnic anchor keys must materialize'
Assert ((Get-SyncVerdict $picnic $ph ($pc -replace 'picnic_heater','other')).Status -eq 'FAIL') 'Missing picnic prop must fail'
$sky = [pscustomobject]@{Name='sky.rotation+moon';Dispatch='Extra'}
$sh = "synctest: EXTRA id=1 name=sky.rotation+moon dispatched=1`n" + ((1..3 | ForEach-Object {'synctest: STATE sky=1 yaw=179.90 moon=0.375'}) -join "`n")
$sc = $sh -replace '179.90','-179.90'
Assert ((Get-SyncVerdict $sky $sh $sc).Status -eq 'PASS') 'Equivalent yaw across the angle wrap must pass'
$invasion=[pscustomobject]@{Name='eggvasion';Dispatch='RunEvent'}
$ih="event_fire: host fire #1 runEvent('eggvasion') origin=dev -- broadcasting`nworld-actor[host ex-enroll]: 'superEgger_C' eid=1`nnpc-sync[ex-spawn]: enrolled 'eg_C' eid=2"
$ic="materialized mirror eid=1 class='superEgger_C'`nmaterialized mirror eid=2 class='eg_C'"
Assert ((Get-SyncVerdict $invasion $ih $ic).Status -eq 'PASS') 'Mixed event actor families must match every identity'
Assert ((Get-SyncVerdict $invasion $ih ($ic -replace "class='superEgger_C'","class='eg_C'")).Status -eq 'FAIL') 'Matching identity with the wrong event actor class must fail'
$pyr=[pscustomobject]@{Name='piramid';Dispatch='RunEvent'}
$yh="event_fire: host fire #1 runEvent('piramid') origin=dev -- broadcasting`nworld-actor[host]: tracked WorldActorSpawn class='piramid2_C' eid=77 loc=(0,0,0)"
$yc="materialized mirror eid=77 class='piramid2_C'"
Assert ((Get-SyncVerdict $pyr $yh $yc).Status -eq 'PASS') 'A piramid2_C world actor materialized by id must pass'
Assert ((Get-SyncVerdict $pyr $yh ($yc -replace 'piramid2_C','piramid_C')).Status -eq 'FAIL') 'The retired piramid_C class name must not satisfy the piramid row'
$food=[pscustomobject]@{Name='food';Dispatch='SpecialEvent'}
$fh="event_fire: host fire #1 runSpecialEvent('food') origin=dev -- broadcasting`nHOST broadcasting SPAWN cls='prop_food_choco_C' key='k1' loc=(0,0,0)`nHOST broadcasting SPAWN cls='prop_food_shrimpburger_C' key='k2' loc=(0,0,0)"
$fc="sync::CreateOrAdoptPropMirror: eid=1 bound to actor=1 key='k1' cls='prop_food_choco_C'`nsync::CreateOrAdoptPropMirror: eid=2 bound to actor=2 key='k2' cls='prop_food_shrimpburger_C'"
Assert ((Get-SyncVerdict $food $fh $fc).Status -eq 'PASS') 'Every prop birth of the row bound on the client must pass'
Assert ((Get-SyncVerdict $food $fh ($fc -replace "key='k2'","key='k3'")).Status -eq 'FAIL') 'One unbound prop birth must fail'
Assert ((Get-SyncVerdict $food ($fh -replace 'prop_food_','prop_drive_') $fc).Status -eq 'FAIL') 'Another class of birth must not pass the row'
$boar=[pscustomobject]@{Name='boarwar';Dispatch='RunEvent'}
$bh="event_fire: host fire #1 runEvent('boarwar') origin=dev -- broadcasting`nHOST broadcasting SPAWN cls='grayboar_C' key='b1' loc=(0,0,0)"
$bc="sync::CreateOrAdoptPropMirror: eid=1 bound to actor=1 key='b1' cls='grayboar_C'"
Assert ((Get-SyncVerdict $boar $bh $bc).Status -eq 'FAIL') 'boarwar without its shooter must stay unproven'
Assert ((Get-SyncVerdict $boar ($bh+"`nnpc-sync[ex-spawn]: enrolled 'npc_ariral_shooter_C' eid=9") ($bc+"`nmaterialized mirror eid=9 class='npc_ariral_shooter_C'")).Status -eq 'PASS') 'boarwar with its props and shooter must pass'
$armed=[pscustomobject]@{Name='gascans';Dispatch='SpecialEvent'}
$ah="event_fire: host fire #1 runSpecialEvent('gascans') origin=dev -- broadcasting"
Assert ((Get-SyncVerdict $armed $ah "event_fire: 'gascans' NOT replayed (prank special (prop lane, overlap-armed: event_funnyGascans_C))").Kind -eq 'Unproven') 'An overlap-armed row is unproven, not a policy gap'
$trap=[pscustomobject]@{Name='atvExplode';Dispatch='SpecialEvent'}
$th="event_fire: host fire #1 runSpecialEvent('atvExplode') origin=dev -- broadcasting`nsynctest: STATE id=1 name=atvExplode trap=1"
Assert ((Get-SyncVerdict $trap $th 'synctest: STATE id=1 name=atvExplode trap=1').Status -eq 'PASS') 'A trap on both peers must pass'
Assert ((Get-SyncVerdict $trap $th 'synctest: STATE id=1 name=atvExplode trap=0').Status -eq 'FAIL') 'A client without the trap must fail'
$guardSource = Join-Path $PSScriptRoot 'ProcessGuard.cs'
if (!('SyncTestProcesses' -as [type])) { Add-Type -Path $guardSource }
$guard = [SyncTestProcesses]::new()
try {
    $envs = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) { $envs[$entry.Key]=$entry.Value }
    $childPid = $guard.Start((Get-Process -Id $PID).Path, '-NoProfile -Command "Start-Sleep -Seconds 60"', $envs)
    $child = Get-Process -Id $childPid
    Assert ($child.PriorityClass -eq 'BelowNormal') 'Guard must set low priority before resume'
    $bits = [Convert]::ToString($child.ProcessorAffinity.ToInt64(),2).Replace('0','').Length
    Assert ($bits -le 4 -and $bits -gt 0) 'Guard must cap affinity at four cores'
} finally { $guard.Dispose() }
Assert (!(Get-Process -Id $childPid -ErrorAction SilentlyContinue)) 'Dispose must kill its own child'
$lockProbe = Join-Path ([IO.Path]::GetTempPath()) ("synctest-lock-check-"+[guid]::NewGuid()+'.lock')
$lockStream = [IO.FileStream]::new($lockProbe,'CreateNew','ReadWrite','None',4096,[IO.FileOptions]::DeleteOnClose)
try {
    $exclusive=$false
    try { $other=[IO.File]::Open($lockProbe,'CreateNew','ReadWrite','None'); $other.Dispose() }
    catch [IO.IOException] { $exclusive=$true }
    Assert $exclusive 'A second rig must not acquire the held lock'
} finally { $lockStream.Dispose() }
Assert (!(Test-Path -LiteralPath $lockProbe)) 'Closing the lock handle must remove its file'
'Verdict self-check PASS (including missing-evidence and fault negative controls)'
