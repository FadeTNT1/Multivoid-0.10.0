# Testing

## Purpose

How to run the test rig, what it can stage, and the gates a push and a release run. The local rig
uses two copies of the game on one machine, driven by one launcher and judged by their logs.

## How it works

### Local 0.10.0 event sweep

Run from the repository root in PowerShell 7:

```powershell
pwsh -NoProfile -File tools/synctest/Run-SyncTest.ps1
```

The launcher builds Release in `build/synctest` with two build jobs, installs the proxy loader
and mod into two private game copies, and enumerates the developer event table plus weather
and sky probes. The source install and its `Mods` are read only. Large cooked content files
are hardlinked; mutable files and the loader are independent copies. The game copies live
under the desktop's `VOTV 0.10.0/rig/host` and `rig/client` folders. Their separate
`synctest_host` and `synctest_client` user-data roots contain only disposable test data. The
engine's saved directory must pass the scenario's isolation check before a world loads. The
real SaveGames directory is backed up beside itself before launching either game.

The explicit `synctest` harness scenario starts a fresh `synctest_world`, binds its host to
loopback and connects one client directly. It does not announce a lobby or start signaling.
The games run hidden and off-screen at 640 by 360, 24 fps, minimum scalability, without sound,
at BelowNormal priority and with at most four logical cores each. A private Windows job kills
only the launcher's processes on exit; a watchdog kills the pair above 6 GiB working set per
game. An exclusive `multivoid-synctest.lock` in the user's local Temp serializes cooperating rigs.
The rig disables launch mouse capture in its private input configuration. Its explicit
`MULTIVOID_HARNESS_NO_INPUT_CAPTURE=1` switch installs Win32 guards before the game's entry
point, blocking cursor clipping, warps, capture and window activation. Windows are minimized
without activation. The launcher checks `GetClipCursor` against the full virtual desktop
before launch and throughout the run, killing its pair if the cursor is clipped or a rig
window becomes foreground. `cursor.log` records the checks.

Reports live under `local/tests/synctest/<run>/`: `report.md`, CSV/JSON results, the enumerated
catalog, full peer logs, per-case log excerpts, and memory and CPU samples. `-ReuseBuild` reuses
this worktree's built DLLs; `-Events solar` selects a case; `-ObserveSeconds 30` extends the
default twelve-second observation; `earthTp` always receives at least 120 seconds for its
delayed sky and cue sequence, and `boarwar` 75, since its first shooter is minted a minute in. A two-second receiver grace covers delivery and log buffering.
`Test-Verdict.ps1` runs missing-evidence, runtime-fault,
unchanged-state and process-containment controls without starting a game.

The sweep uses one cumulative fresh world. A PASS proves the named native replay, matching
entity identity, every host prop birth of the row's own classes bound on the client by key, or
sampled weather, sky or ATV-trap state; it does not establish delayed effects, all
visuals, audibility or late joins. FAIL distinguishes a measured mismatch, an explicitly
host-local/missing lane, insufficient effect evidence, and a blocked launch. Native events
that only arm an overlap are unproven, and a row the policy calls overlap-armed says so. Firefly ticks retain their native night/RNG
gates. Flesh-rain and gift rows expose missing deterministic drivers rather than silently claiming
coverage. The alarm probes assert both broadcast and native replay of each edge. Church and
UFO-controller internal schedules need separate scenario drivers; the event table covers their
addressable eventer routes only.

### Other harness workers

The DLL contains additional environment-gated workers for joins, reload churn, fog, player and
prop interactions, devices and screenshots. Their gates and thread entry points are listed in
`harness/autotest/autotest_dispatch.cpp`. The local event launcher selects only its dedicated
`synctest` scenario; those workers require their own controllers and assertions.

### The in-game harness

The harness ships inside the DLL (`harness/`). At load it reads `VOTVCOOP_SCENARIO` from the
process environment; an absent value selects the native menu. The scenario posts engine actions
onto the game thread: `play` loads the configured save, `menu` keeps the native menu, and
`synctest` starts the isolated event driver. Environment variables override the ini for one launch (role, port, peer,
nickname, spawn pose), which is how the launcher configures a peer without editing files. Each
scenario is one file under `harness/autotest/`, armed by its own environment gate. A scenario
drives the engine through reflected function calls and walks a bot by asking the game for a path
to a target, never through synthesised input.

### Logs and assertions

Every copy writes a levelled, timestamped `multivoid.log` beside its game executable, and a scenario's verdict
is a line in it. One log assertion checks the pile carry-and-throw loop against thirteen
invariants across both peers' logs and prints a verdict table; a capture step
grabs a game window from outside the process for the screenshot scenarios.

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
| A PASS covers only the native replay, mirrored identity or sampled state named by the assertion; it does not prove every delayed or visible effect | `[V]` `tools/synctest/Verdict.ps1` |
| The local event sweep writes structured reports; other scenarios still emit their verdict in a log | `[V]` `tools/synctest/Run-SyncTest.ps1` |

## Code map

| Concept | Files |
|---|---|
| the local event launcher and verdict controls | `tools/synctest/Run-SyncTest.ps1`, `tools/synctest/Verdict.ps1`, `tools/synctest/Test-Verdict.ps1`, `tools/synctest/ProcessGuard.cs` |
| the in-game harness | `harness/harness`, `harness/session_runtime`, `harness/autotest`, `harness/autotest_dispatch`, `harness/autotest/`, `harness/sdk_check`, `harness/screenshot` |
| the gates | `.github/ci/abi_gate.py`, `.github/ci/public_prose_gate.py`, `.github/ci/public_leak_gate.py`, `.github/ci/commit_msg_check.py`, `.github/workflows/build-core.yml`, `.github/workflows/repo-gates.yml`, `.github/workflows/release-core.yml` |
