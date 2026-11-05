# Testing

## Purpose

How to run the test rig, what it can stage, and the gates a push and a release run. The rig is two
to four copies of the game on one machine, driven by one launcher and judged by their logs.

## How it works

### The rig

Four copies of the game live beside the repository, each with its own saved games and logs: a
host, two clients and a fourth kept for development. A deploy step copies the built DLL into every
copy's mod folder, skipping a copy whose bytes already match; a separate one-time step installs the
pinned UE4SS loader into a copy, verifying its
hash. Nothing in a game copy is tracked.

### The launcher

One launcher deploys and launches peers and runs scenarios. `host`, `client`, `client2` and
`client3` launch one peer each for playing; `kill` stops every game process. Every step
prints a line as it happens, and the game is launched detached so the launcher never holds its
pipes.

The smoke is the standing regression check. It deploys, launches the host windowed, waits for it to bind its port, launches a client, samples
every process's memory on an interval, kills everything if a peer crosses the memory threshold,
tails both logs, kills both peers, and passes only if both were alive at the last sample. Its own
verdict is liveness and memory; feature verdicts are read from the logs. `smoke4` runs a host and
three clients with a staggered connect and proves the host relay end to end by finding, in each
client's log, a puppet spawned for another client.

The other subcommands stage one scenario each:

| Area | Subcommands |
|---|---|
| joins and the world | `joinchurn`, `reloadchurn`, `menutravel`, `wirewindow`, `fogprobe`, `deadmaster`, `gracefulexit`, `authdrill` |
| players, entities and props | `npctest`, `death`, `ragdollshot`, `ragdollspawn`, `puppetshot`, `walkgrab`, `clumpvis`, `spawnmenutest`, `navprobe` |
| devices and races | `lightgroup`, `ctakeprobe`, `ctakerace` |
| screens and captures | `browser`, `nativeui`, `menushot`, `scoreshot`, `hudtint`, `chathistory`, `chatseed` |
| variants of the smoke | `smoke_phystele`, `smoke_i18n` |

`ctakerace` is the shape a concurrency test takes here: the host and a client both walk to the
same container and take the same item at one GO barrier (a future-timestamp sentinel file, which
gives sub-millisecond simultaneity on one machine); each peer counts the item afterwards and the
launcher sums across peers, so one is correct, two is a duplicate and zero is a loss. Its
`control` mode, where only the host takes, must sum to one before a race result is trusted.

### The in-game harness

The harness ships inside the DLL (`harness/`). At load it reads a scenario from a file beside the
mod or from the environment, and the scenario selects a code path that posts engine actions onto
the game thread: `play` loads a save and idles, `load:<slot>` loads a named slot, `none` launches
with no automation. Environment variables override the ini for one launch (role, port, peer,
nickname, spawn pose), which is how the launcher configures a peer without editing files. Each
scenario is one file under `harness/autotest/`, armed by its own environment gate. A scenario
drives the engine through reflected function calls and walks a bot by asking the game for a path
to a target, never through synthesised input.

### Logs and assertions

Every copy writes a levelled, timestamped `multivoid.log` beside the mod, and a scenario's verdict
is a line in it. One log assertion checks the pile carry-and-throw loop against thirteen
invariants across both peers' logs and prints a verdict table; a capture step
grabs a game window from outside the process for the screenshot scenarios.

The sync log analyzer ranks faults, dead watches, fingerprint drift, join aborts and missing
applications across peer logs, and writes JSON with source line references. Its environment-gated
error scenarios exercise mid-alarm join, rejoin, save/reload, disconnect and the existing container
race in isolated test copies. Run instructions and limits follow below.

### The gates a push runs

Every push and pull request runs the source gates, which need no compiler and take about ten
seconds (`.github/workflows/repo-gates.yml`): the config registry, the peer-slot generation, the
master contact (nothing tells an outside server where a player is unasked), the reliable-kind
routing (every wire kind reaches a receiver or says why not), the MinHook free-call rule, the GC
pin ownership, and the font atlas regime.

The Windows build is run by hand (Actions -> **build** -> Run workflow), because a twenty-minute
runner on every push buys little the source gates do not already cover. It checks out the
requested source, initialises only the third-party submodules, re-runs those same gates, then
restores the pinned dependency cache, builds Release, runs the zero-import ABI gate on the built
DLL, packages and verifies the mod zip, and uploads the artifacts. A second workflow gates the repository itself: the public-leak gate, the
commit-message check over every commit since the checker was added, and the public-prose gate,
each preceded by a drill that proves the gate can fail.

A release is a tag: the release workflow judges the tag against the ledger, refuses early on a
stale fingerprint, rebuilds the tagged source without caches, and publishes the zip with its
checksum ([release.md](release.md)).

## Known limits

| Limit | Evidence |
|---|---|
| The rig stages what a scenario scripts: a bot walks to a chosen target and performs the scripted action. Nobody plays the game freely, so a mechanic's feel and anything visual are judged by a person | `[V]` `harness/autotest/` |
| A race is staged only where a scenario provides a barrier; the container take is the one that has it, other concurrent interactions have no scenario yet | `[V]` `harness/autotest/` (the `ctakerace` scenario) |
| The smoke's own verdict is liveness and memory; a regression that keeps both peers alive passes it unless a feature assertion reads the logs | `[V]` by design |
| The sync analyzer writes structured JSON; lanes without paired application markers still require their own log assertions | `[V]` `tools/synctest/analyze/` |

## Code map

| Concept | Files |
|---|---|
| the in-game harness | `harness/harness`, `harness/session_runtime`, `harness/autotest`, `harness/autotest_dispatch`, `harness/autotest/`, `harness/sdk_check`, `harness/screenshot` |
| the gates | `.github/ci/abi_gate.py`, `.github/ci/public_prose_gate.py`, `.github/ci/public_leak_gate.py`, `.github/ci/commit_msg_check.py`, `.github/workflows/build-core.yml`, `.github/workflows/repo-gates.yml`, `.github/workflows/release-core.yml` |

## Sync log analysis

Run with Python 3.10 or newer; no packages are needed:

```powershell
python tools/synctest/analyze/analyze_sync_logs.py --host "$logDir/host/multivoid.log" --client "$logDir/client/multivoid.log" --json "$logDir/summary.json"
```

Repeat `--client` for more peers. `--top 30` prints more signatures; JSON always contains all
signatures, occurrence counts, source line ranges, examples, and deduplicated caller frames.
`--fail-on-error` returns 1 for errors, 0 otherwise; invalid inputs return 2. Without this switch,
finding failures does not change the exit code. Inputs are opened read-only.

Exceptions rank first, then dead watches, fingerprint drift, aborts, desyncs, and ordinary warnings.
The signature keeps module offsets, exception codes, watch names and fingerprint keys, while
removing process addresses. Four caller lines following one fault are evidence for that fault,
not four additional faults. A zero-filled or truncated log tail is tolerated.

Cross-peer checks compare EventFire, AlarmState and WeatherState sends with application markers
within `--grace-seconds 30`. A queued receive is not an application. An explicit event no-replay
policy is accepted as intentional handling. Both logs must identify the same DLL hash/build, have
overlapping clocks, and show client world readiness. Different historical builds are reported as
skipped, not as desyncs. Supply logs from one run: log format 1 has no date or connection identity.
Other lanes still surface their explicit ERROR/WARN/desync lines, but have no delivery proof.

Memory is measured only when a log contains working-set samples. EOF does not prove process death.
Optional `--telemetry samples.csv` accepts `pid,working_set_bytes,alive,time` columns (`time` is
optional); `alive` is true/false or 1/0. PIDs must match supplied logs. Working sets above 6 GiB and
dead-process samples are critical. This analyzer never launches, changes, or kills a process.

Run the parser's regression checks with:

```powershell
python -m unittest discover -s tools/synctest/analyze -v
```

### Adversarial event scenarios

The harness dispatch compiles `synctest_err_scenarios.cpp` into the normal DLL. With the environment
gate absent it does nothing. Set these variables on both peers before a `play` launch:

| Variable | Value |
|---|---|
| `VOTVCOOP_SYNCTEST_ERR` | `join`, `rejoin`, `reload`, `race`, or `disconnect` |
| `VOTVCOOP_SYNCTEST_ERR_DIR` | A new, existing, absolute directory shared by this pair only |
| `VOTVCOOP_SAVE` | An isolated existing world whose name begins with `synctest_` |
| `VOTVCOOP_NET_ROLE` | `host` or `client`, respectively |
| `VOTVCOOP_NET_PEER` | `127.0.0.1` on the client |
| `VOTVCOOP_NET_PORT` | The same unused direct UDP port on both peers |
| `VOTVCOOP_SCENARIO` | `play` |

Use the rig launcher when available. For a hand run, use two copies below the `VOTV 0.10.0/rig/`
directory with separate engine save roots, for example `-saveddirsuffix=synctest_host` and
`-saveddirsuffix=synctest_client`. The scenario verifies the engine-resolved save directory contains
`synctest_` and is outside the normal `VotV/Saved/SaveGames` tree. It refuses unprefixed world names,
non-rig executables, and reused barrier directories. Never use the original game install or saves.

The launcher/operator must hold an exclusive FileStream on
`$env:TEMP/multivoid-synctest.lock` for the entire pair, waiting if another run owns it, then dispose
and remove it after both owned processes exit. Start each game minimized without activation
(`SW_SHOWMINNOACTIVE`) with `-windowed -ResX=640 -ResY=360`, BelowNormal/Idle priority, at most four cores, lowest scalability
and a 20–30 FPS cap. Sample working sets externally and stop only owned PIDs if one exceeds 6 GiB.
The scenarios are in-process drivers, not launchers or resource monitors.

In each copy's Input.ini under its isolated `-UserDir`, configure:

```ini
[/Script/Engine.InputSettings]
bCaptureMouseOnLaunch=False
DefaultViewportMouseCaptureMode=NoCapture
DefaultViewportMouseLockMode=DoNotLock
```

Verify `GetClipCursor` remains the full virtual desktop while each game runs. Stop the owned
games immediately if either captures input, clips the mouse, or activates its window. Do not
relaunch until the rig's input containment fix is in place and verified.

| Scenario | Procedure and evidence |
|---|---|
| `join` | Start the host first. Wait for `synctest_err: EVENT_ACTIVE` or the `event-on` file before launching the client. The native alarm and `getEvent=On` prove an event was already running. The client must apply ON after joining, then OFF. |
| `rejoin` | Start both peers. After the client observes ON, it stops its session and travels to the menu. The host must observe departure while retaining its event, then the same client process reconnects directly. It must observe ON again and then OFF. |
| `reload` | Start both. While the event is ON, the host captures live world state to an isolated `synctest_err_reload_<pid>` scratch save, ends its session and reloads that save through the native loader. The new world must retain the active alarm, and the client must reach the menu cleanly. This is a save/reload continuity check; it does not automatically rehost. Scratch saves are retained for inspection. |
| `disconnect` | Start both. The client leaves while the alarm runs. The host must retain its session and active event across departure and a further poll, then end the alarm. |
| `race` | Both peers run the existing container concurrent-take drill while the alarm runs. Follow its GO barrier and read its conservation verdict. The wrapper reports STAGED, never PASS on the strength of a returned worker. |

The base alarm is a native in-flight event (`lib_C::setEvent`) with an idempotent trigger and an
explicit late-join snapshot. These scenarios cover this event's lifecycle, not every story event's
replay policy. Every milestone wait has a 240-second budget. A stalled engine task may still need
the launcher's external timeout/resource guard. Missing verdicts or timed-out milestones fail the run.

For `race`, also set `VOTVCOOP_RACE_ROLE=host|client`, `VOTVCOOP_RACE_MODE=control|race`, and
`VOTVCOOP_RACE_GO_FILE` to an absent file in the fresh barrier directory. Leave the separate
`VOTVCOOP_RUN_CTAKE_RACE` gate unset so the drill is not started twice. After **both** logs print
`director/ctake-race: ARRIVED`, write a future Unix timestamp in milliseconds to the GO file:

```powershell
([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() + 3000).ToString() | Set-Content -LiteralPath $goFile -Encoding ascii
```

Run a fresh control with `VOTVCOOP_RACE_TAKER=host`, another fresh control with `client`, then a
fresh race. Trust the race only if both control verdicts passed. Read each `RESULT ... total=...
verdict=...` line; summing `localCountAfter` counts mirrors twice. A successful wrapper alone is
not a conservation verdict.

### Recognizing the old menu-pump fault

The repeated `PostMenuTick` fault at `VotV-Win64-Shipping.exe+0x136CA24`, with caller RVAs
`13D6BC`, `13E1FC`, `315D65`, `314938`, follows name rendering, `ToString`, key indexing and
admission during the key-index seed. The native instruction reads an FName-entry header; an access
address of `0x2` is an invalid entry, not a failed network send. The old 0.9 key offset read flag
bytes as an FName after the 0.10.0 layout moved by eight bytes. The seed never finishes and the next
menu pump attempts the same seed again.

Commit `382f3af42` already changed `sdk_profile.h`'s `Aprop_Key` from `0x2E0` to `0x2E8` and added
the live SDK offset audit. Deploy a DLL containing that fix to both test copies. Do not hide this
with a catch, a skipped key, or a disabled watch. Historical RVAs cannot be looked up blindly in a
new DLL's map/PDB: compare the matching image or unchanged function bodies and call sites first.
