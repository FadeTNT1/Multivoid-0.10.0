function Get-SyncVerdict($Row, [string]$HostText, [string]$ClientText) {
    function Verdict($status, $kind, $evidence) {
        if ($status -eq 'FAIL' -and $kind -eq 'Unproven') {
            $last = ($HostText + "`n" + $ClientText) -split "`r?`n" | Where-Object {
                $_ -match 'event_fire:|event_trigger:|synctest: (TRIGGER|EXTRA)'
            } | Select-Object -Last 1
            if ($last) { $evidence += "; last event line: $last" }
        }
        [pscustomobject]@{ Name = $Row.Name; Status = $status; Kind = $kind; Evidence = $evidence }
    }
    $fault = ($HostText + "`n" + $ClientText) -split "`r?`n" | Where-Object { $_ -match 'posted task.*FAULT|\[(ERROR|E)\s*\]' } | Select-Object -First 1
    if ($fault) { return Verdict FAIL Fault $fault }
    $name = [regex]::Escape($Row.Name)
    if ($Row.Dispatch -ne 'Ambient' -and $Row.Dispatch -ne 'Extra') {
        $wireName = if ($Row.Dispatch -eq 'RandomPrank') { 'arirInteraction_0' } else { $Row.Name }
        $escaped = [regex]::Escape($wireName)
        $fire = $HostText -split "`r?`n" | Where-Object { $_ -match "event_fire: host fire #\d+ .*\('$escaped'\)" } | Select-Object -First 1
        if (!$fire) { return Verdict FAIL Unproven "No completed host fire for '$wireName' in this case window" }
        if ($Row.Dispatch -eq 'RandomPrank') {
            $pick = [regex]::Match($HostText, "event_fire: host fire #\d+ runSpecialEvent\('([^']+)'\)")
            if (!$pick.Success) { return Verdict FAIL Unproven 'Random prank selected no observable special in case window' }
            $picked = Get-SyncVerdict ([pscustomobject]@{Name=$pick.Groups[1].Value;Dispatch='SpecialEvent'}) $HostText $ClientText
            return Verdict $picked.Status $picked.Kind "Random pick '$($picked.Name)': $($picked.Evidence)"
        }
        $noLane = $ClientText -split "`r?`n" | Where-Object { $_ -match "event_fire: '$escaped' NOT replayed.*(no lane|host-local|per-player teleport)" } | Select-Object -First 1
        if ($noLane) { return Verdict FAIL PolicyGap $noLane }
        # An overlap-armed row produces nothing until a pawn walks into its box, which this driver never does.
        $armed = $ClientText -split "`r?`n" | Where-Object { $_ -match "event_fire: '$escaped' NOT replayed.*overlap-armed" } | Select-Object -First 1
        if ($armed) { return Verdict FAIL Unproven "Overlap-armed (the rig drives no walk-in): $armed" }
        if ($Row.Name -eq 'atvExplode') {
            $hs = @([regex]::Matches($HostText, 'synctest: STATE[^\r\n]+') | ForEach-Object Value)
            $cs = @([regex]::Matches($ClientText, 'synctest: STATE[^\r\n]+') | ForEach-Object Value)
            if ($hs.Count -and $cs.Count -and $hs[-1] -match '\btrap=1\b' -and $cs[-1] -match '\btrap=1\b') {
                return Verdict PASS State "Host fire; the gamemode ATV's trap reads 1 on host and client (ATV stamp bit 4)"
            }
            $last = if ($cs.Count) { $cs[-1] } else { 'no client STATE' }
            if ($hs.Count -and $hs[-1] -match '\btrap=1\b') { return Verdict FAIL Desync "Host trap=1, client did not follow: $last" }
            return Verdict FAIL Unproven "The host's gamemode ATV never read trap=1 in the case window: $last"
        }
        if ($Row.Name -eq 'starRain' -and $HostText.Contains("event_cue: host broadcast 'starRain' (cue 0)") -and $ClientText.Contains("event_cue: replayed 'starRain' (cue 0)")) {
            return Verdict PASS Mirror 'Host broadcast starRain cue 0; client event_cue: replayed starRain cue 0'
        }
        if ($Row.Name -eq 'earthTp') {
            $cue = [regex]::Match($HostText, 'sky_sync: host earthTp[^\r\n]*cue #(\d+) sent')
            $seq = $cue.Groups[1].Value
            if ($cue.Success -and $ClientText -match "host earthTp cue #$seq -> start sound played" -and
                $ClientText.Contains('sky_sync: earthTp end sound played') -and
                $ClientText.Contains("sky_sync: applied the host's black-hole sky 1") -and
                $ClientText.Contains("sky_sync: applied the host's black-hole sky 0")) {
                return Verdict PASS State "Host earthTp cue #$seq; client start/end sound dispatch and black-hole sky 1 -> 0 observed"
            }
            return Verdict FAIL Unproven 'earthTp full cue/sky sequence not observed (requires at least 120 seconds; audibility is disabled)'
        }
        if ($Row.Name -eq 'picnic' -and $HostText.Contains("key='wa2_picnic'")) {
            $props = [regex]::Matches($HostText, "HOST broadcasting SPAWN cls='(?:prop_C|prop_heater_C|prop_watoy_C)' key='([^']+)'")
            $missing = @($props | Where-Object { $ClientText -notmatch ("CreateOrAdoptPropMirror:.*bound to actor=.*key='"+[regex]::Escape($_.Groups[1].Value)+"'") })
            if ($missing.Count) { return Verdict FAIL Desync "$fire; client missing picnic prop key '$($missing[0].Groups[1].Value)'" }
            if ($props.Count -ge 2 -and $HostText.Contains("key='picnic_heater'")) { return Verdict PASS Mirror "All $($props.Count) picnic prop keys materialized, including wa2_picnic and picnic_heater" }
        }
        if ($Row.Name -eq 'destroyPicnic' -and $HostText.Contains("key='wa2_picnic'")) {
            $timestamp = [regex]::Match($fire, '^\[([^]]+)\]').Groups[1].Value
            if ($timestamp) {
                $deletes = [regex]::Matches($HostText, '\['+[regex]::Escape($timestamp)+"\][^\r\n]*HOST broadcasting DESTROY[^\r\n]*key='([^']+)' eid=(\d+)")
                $missing = @($deletes | Where-Object { $ClientText -notmatch ("remote_prop::OnDestroy: key '"+[regex]::Escape($_.Groups[1].Value)+"' eid="+$_.Groups[2].Value+" -> destroying local actor") })
                if ($missing.Count) { return Verdict FAIL Desync "$fire; client did not destroy picnic-window prop key '$($missing[0].Groups[1].Value)' eid=$($missing[0].Groups[2].Value)" }
                if ($deletes.Count) { return Verdict PASS Mirror "All $($deletes.Count) host prop destroys in the event's second applied on client, including wa2_picnic" }
            }
        }
        if ($ClientText -match "event_fire: client REPLAY .* '$escaped'" -and $ClientText -match "event_fire: run(?:Special)?Event\('$escaped'[^\r\n]* dispatched") {
            return Verdict PASS Replay "Host: $fire; client native replay dispatched (effect/visuals not asserted)"
        }
        # Entity ids, rather than unrelated spawn counts, correlate the lane output across peers.
        $classes = @{
            ventCrawler='ventCrawler_C'; ventKnocker='kocker_C'; tentacleBalls='tentacleBall_C'
            morningGay='morningUfo_C'; borgRozital='rozitBorg_C'; fallbody_0='ufoDropper_body_C'
            fallbody_1='ufoDropper_body_C'; fallcar_0='ufoDropper_car_C'; wisps='wisp_C'; piramid='piramid_C'
            arirFollower='npc_arirFollower_C'; eggvasion=@('superEgger_C','eg_C')
            graystank=@('ufoDropper_tank_C','ufoDropper_pig_C'); boarwar='npc_ariral_shooter_C'
        }
        # Rows whose output is props born on the host: every host prop birth of the row's own classes in the
        # window must bind on the client by key. Classes are the row's, so a background spawn cannot pass it.
        $propClasses = @{
            food='prop_food_\w+'; drive='prop_drive_C'; poisonFood='prop_food_\w*[pP]oison\w*'
            expDrive='prop_drive_explosive_C'; cookiebox='prop_cookiebox_C'; bombBox='prop_bombbox_C'
            begos='prop_bego_spikeball_C'; boarwar='grayboar_C'
            arirBuster='prop_container_crate_C|prop_lamp_blinker_C|prop_notebook_busterSpawner_C'
        }
        $propVerdict = $null
        if ($propClasses.ContainsKey($Row.Name)) {
            $births = @([regex]::Matches($HostText, "HOST broadcasting SPAWN cls='(?<class>(?:$($propClasses[$Row.Name])))' key='(?<key>[^']+)'"))
            if (!$births.Count) { return Verdict FAIL Unproven "Host fired '$wireName'; no host prop birth of its classes in the case window" }
            $lost = @($births | Where-Object { $ClientText -notmatch ("CreateOrAdoptPropMirror:[^\r\n]*bound to actor=[^\r\n]*key='"+[regex]::Escape($_.Groups['key'].Value)+"'") })
            if ($lost.Count) { return Verdict FAIL Desync "$fire; client never bound prop key '$($lost[0].Groups['key'].Value)' ($($lost[0].Groups['class'].Value)), $($lost.Count) of $($births.Count) missing" }
            $kinds = ($births | ForEach-Object { $_.Groups['class'].Value } | Sort-Object -Unique) -join ','
            $propVerdict = "all $($births.Count) host prop births ($kinds) bound on client by key"
        }
        $ids = if ($classes.ContainsKey($Row.Name)) {
            $cls = '(?:'+(@($classes[$Row.Name] | ForEach-Object { [regex]::Escape($_) }) -join '|')+')'
            [regex]::Matches($HostText, "(?:tracked (?:EntitySpawn|WorldActorSpawn) class='(?<class>$cls)'[^\r\n]*?(?:elementId|eid)=|world-actor\[host ex-enroll\]: '(?<class>$cls)' eid=|npc-sync\[ex-spawn\]: enrolled '(?<class>$cls)' eid=)(?<eid>\d+)")
        } else { @() }
        $matched = @(); $missing = @()
        foreach ($spawn in @($ids | Group-Object { $_.Groups['eid'].Value } | ForEach-Object { $_.Group[0] })) {
            $eid = $spawn.Groups['eid'].Value
            $exactClass = [regex]::Escape($spawn.Groups['class'].Value)
            $mirror = $ClientText -split "`r?`n" | Where-Object { $_ -match "materialized mirror eid=$eid\b[^\r\n]*class='$exactClass'" } | Select-Object -First 1
            if ($mirror) { $matched += $eid } else { $missing += $eid }
        }
        if ($missing.Count) { return Verdict FAIL Desync "$fire; host spawn: $($ids[0].Value); client missing class '$($classes[$Row.Name])' entity ids: $($missing -join ',')" }
        if ($matched.Count) {
            $alsoProps = if ($propVerdict) { "; $propVerdict" } else { '' }
            return Verdict PASS Mirror "Host fire; all $($matched.Count) observed '$($classes[$Row.Name])' spawns materialized on client, ids=$($matched -join ',')$alsoProps"
        }
        if ($propVerdict -and $classes.ContainsKey($Row.Name)) {
            return Verdict FAIL Unproven "Host fire; $propVerdict; no '$($classes[$Row.Name] -join '/')' spawn in the case window"
        }
        if ($propVerdict) { return Verdict PASS Mirror "Host fire; $propVerdict (prop lane)" }
        return Verdict FAIL Unproven "Host fired '$wireName'; no matching native replay or materialized entity in case window"
    }
    $trigger = if ($Row.Dispatch -eq 'Extra') { "synctest: EXTRA .*name=$name dispatched=1" } else { "event_trigger: ambient .* (?:dispatched \('$name'\)|superFog spawned)" }
    if ($Row.Name -eq 'superFog') { $trigger = 'event_trigger: ambient superFog spawned' }
    if ($HostText -notmatch $trigger) { return Verdict FAIL Unproven "No successful native trigger for '$($Row.Name)'" }
    $hs = @([regex]::Matches($HostText, 'synctest: STATE[^\r\n]+') | ForEach-Object Value)
    $cs = @([regex]::Matches($ClientText, 'synctest: STATE[^\r\n]+') | ForEach-Object Value)
    if ($Row.Name -in 'alarm ON','alarm OFF') {
        $active = if ($Row.Name -eq 'alarm ON') { 1 } else { 0 }
        if ($HostText -match "alarm_sync: host broadcast active=$active" -and $ClientText -match "alarm_sync: applied active=$active") {
            return Verdict PASS State "Host broadcast active=$active; client alarm_sync: applied active=$active (native runTrigger replay)"
        }
        return Verdict FAIL Unproven "No matching host alarm transition and client replay for active=$active"
    }
    if ($Row.Name -eq 'wind' -and $hs.Count -ge 3 -and $cs.Count -ge 3) {
        $pattern = 'wind=1 wx=([-\d.]+) wy=([-\d.]+) wz=([-\d.]+)'
        $hw = [regex]::Match($hs[-1],$pattern); $cw = [regex]::Match($cs[-1],$pattern)
        if ($hw.Success -and $cw.Success -and $hw.Value -eq $cw.Value) { return Verdict PASS State "Native wind roll; matching $($hw.Value)" }
        return Verdict FAIL Desync "Wind mismatch: $($hs[-1]); CLIENT $($cs[-1])"
    }
    $field = switch ($Row.Name) {
        'rain ON' { 'flags'; break } 'rain OFF' { 'flags'; break }
        'snow ON' { 'flags'; break } 'snow OFF' { 'flags'; break }
        'spawnFog' { 'fog'; break } 'superFog' { 'fog'; break } 'spawnBlackFog' { 'fog'; break }
        'spawnRedSky' { 'red'; break } 'badSun' { 'sun'; break }
        'sky.eye ON' { 'eye'; break } 'sky.eye OFF' { 'eye'; break }
        'sky.blackhole ON' { 'hole'; break } 'sky.blackhole OFF' { 'hole'; break }
        'sky.rotation+moon' { 'moon'; break } 'jellyfish' { 'fish'; break }
        default { '' }
    }
    if ($Row.Name -eq 'lightning' -and $HostText -match 'host broadcast LightningStrike' -and $ClientText -match 'lightning Apply spawned') {
        return Verdict PASS Mirror 'Host broadcast LightningStrike; client lightning Apply spawned'
    }
    if ($Row.Name -eq 'fireflies') {
        $spawns = [regex]::Matches($HostText, 'firefly: broadcast own spawn at (\([^\r\n]+\))')
        foreach ($spawn in $spawns) {
            if ($ClientText.Contains("firefly: spawned peer's eff_fireflies at $($spawn.Groups[1].Value)")) {
                return Verdict PASS Mirror "Host firefly spawn and client emitter at $($spawn.Groups[1].Value)"
            }
        }
        return Verdict FAIL Unproven 'Native firefly ticks attempted; no matching emitted particle location (night/RNG dependent)'
    }
    if ($field -and $hs.Count -ge 3 -and $cs.Count -ge 3) {
        $hv = [regex]::Match($hs[-1], "\b$field=([-\d.]+)").Groups[1].Value
        $cv = [regex]::Match($cs[-1], "\b$field=([-\d.]+)").Groups[1].Value
        $expected = switch ($Row.Name) {
            'rain ON' { 1 } 'rain OFF' { 0 } 'snow ON' { 2 } 'snow OFF' { 0 }
            'spawnFog' { 1 } 'superFog' { 2 } 'spawnBlackFog' { 16 }
            'spawnRedSky' { 1 } 'badSun' { 1 } 'jellyfish' { 7 }
            'sky.rotation+moon' { 0.375 }
            default { if ($Row.Name.EndsWith(' ON')) { 1 } else { 0 } }
        }
        $mask = switch ($field) { 'flags' { if ($Row.Name.StartsWith('rain')) { 1 } else { 2 } } 'fog' { $expected } default { 0 } }
        $ok = if ($mask) { (([int]$hv -band $mask) -eq $expected) -and (([int]$cv -band $mask) -eq $expected) } else { $hv -ne '' -and $hv -eq $cv -and [double]$cv -eq $expected }
        if ($ok -and $Row.Name -eq 'sky.rotation+moon') {
            $hy=[double][regex]::Match($hs[-1], '\byaw=([-\d.]+)').Groups[1].Value
            $cy=[double][regex]::Match($cs[-1], '\byaw=([-\d.]+)').Groups[1].Value
            $yawDelta=[Math]::Abs((($hy-$cy+540)%360)-180)
            $ok=$hs[-1].Contains('sky=1 ') -and $cs[-1].Contains('sky=1 ') -and $yawDelta -lt 2
        }
        if ($ok -and $Row.Name -eq 'jellyfish') {
            $fish = @([regex]::Matches($HostText, "(?:WorldActorSpawn class='jellyfish_C'[^\r\n]*eid=|world-actor\[host ex-enroll\]: 'jellyfish_C' eid=)(\d+)") | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
            if ($fish.Count -ne 7) { return Verdict FAIL Unproven "Native jellyfish trigger completed; expected seven newly observed host actor ids, found $($fish.Count)" }
            foreach ($eid in $fish) {
                if ($ClientText -notmatch "materialized mirror eid=$eid\b[^\r\n]*class='jellyfish_C'") { return Verdict FAIL Desync "Host jellyfish eid=$eid never materialized on client" }
            }
        }
        if ($ok -and $Row.Name.StartsWith('rain')) {
            $hr=[double][regex]::Match($hs[-1], '\brain=([-\d.]+)').Groups[1].Value
            $cr=[double][regex]::Match($cs[-1], '\brain=([-\d.]+)').Groups[1].Value
            $ok=[Math]::Abs($hr-$cr) -le 0.05
        }
        if ($ok) { return Verdict PASS State "Matching observed state: $field host=$hv client=$cv expected=$expected" }
        return Verdict FAIL Desync "State mismatch/trigger no-op: $($hs[-1]); CLIENT $($cs[-1])"
    }
    return Verdict FAIL Unproven "Native trigger completed; no deterministic mirror assertion for '$($Row.Name)'"
}
