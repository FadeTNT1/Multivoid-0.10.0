# CLAUDE.md — agent guide for Multivoid

Read first: `README.md`, `CONTRIBUTING.md`, `docs/architecture.md`, `docs/code-map.md`.
Porting to a new game build: `docs/versioning.md` (the runbook). Build: `BUILDING.md`.

## Current targets

- Game: **Voices of the Void 0.10.0** (`a0.10.0_0009`). 0.9.0n is kept only as a known older
  row in `ue_wrap/facts/game_builds.inc`.
- Loader: Multivoid's **own proxy loader** (`dwmapi.dll` beside the game exe). UE4SS is no
  longer required; the UE4SS C-ABI (`start_mod`) stays so a UE4SS install still works.
- Friend installer: `installer/` (PowerShell: 1 install, 2 uninstall, 3 repair).
- Dedicated server: headless host for Pterodactyl and similar panels — `docs/dedicated-server.md`.

## Model policy (token budget matters — pick the cheapest model that can do the job)

| Model | Use for | Never for |
|---|---|---|
| **GPT-6.1 Sol** (Codex provider, `gpt-6.1-sol`) | research, file exploration, quick lookups, decoding a dump/log/binary format, answering "where is X" | real builds, writing or editing code |
| **Sonnet** | quick bug fixes, small/medium code implementation, scripts, mechanical edits | big cross-system design or ports |
| **Opus** | big runs (a game-version port, a new subsystem like the dedicated server), or a last Hail Mary when Sonnet failed twice | routine edits, lookups |

Rules:
- **Never use 5.6 models** (they hang). GPT: only 6.1 / 6 families; prefer GPT-6.1 Terra and
  GPT-6.1 Sol. Luna is allowed but rare; for cheap work prefer Sonnet or Haiku over Luna.
  See `AGENTS.md`.
- Every sub-agent / delegated child must invoke the `caveman:caveman` skill (full) first thing,
  and its brief must say so. Code, commits and docs stay normal prose.
- Explore with GPT-6.1 Sol or a read-only locator before reading many files yourself; bring back
  conclusions (file:line), not file dumps.
- Delegate with a tight brief: goal, files, what is already ruled out, done-criteria.
- One authority per task: do not run the same investigation twice in parallel.
- A child never commits to `main`; work in a worktree/branch and report what changed.
- Finished, built, gate-passing work lands on the current working branch (`port/0.10.0`) in the
  main checkout. Do not leave it parked on side worktree branches.

## Verify the user's diagnosis — evidence over assumption

The user's bug reports are observations plus a guess at the cause. Treat the observation as
real and the guess as a hypothesis, not as fact.

- Before acting on a suspected cause, check it against evidence: the game log
  (`multivoid.log` beside the game exe), the code, git history, screenshots/video.
- Example: "syncing is broken for X" has more than once turned out to be one shared cause —
  e.g. the script-gate name table filling up (`script_gate: name watch table full`) and
  silently killing every hook registered after it. Grep the log for `ERROR`, `dead`,
  `table full` before blaming the feature itself.
- It is always fine to disagree with the user or say they are wrong — but only with real
  evidence (log line, file:line, commit). Quote it.
- If there is no evidence either way, keep the user's idea as one possibility, investigate,
  and say plainly what was confirmed and what was not.
- Only accept a claim as-is when it is consistent with the evidence and sounds technically sound.
- Pass this rule on to every sub-agent: briefs must say "fact-check the stated cause first."

## Act, don't ask

- "Investigate X" means investigate **and fix** X. When research finds the cause, spawn the fix
  agent right away (cheapest model that can do it, per the policy above). Do not end with
  "want me to fix it?" or "should I spawn an implementer?".
- Ask only when a choice is genuinely the user's (gameplay design, destructive or outward-facing
  actions). Otherwise pick the sensible default, say what you picked, and proceed.
- Sequence agents that touch the same files or both bump `kProtocolVersion`; run independent ones
  in parallel (worktrees), then merge into the working branch.

## Hard rules (from CONTRIBUTING.md, summarised)

- `ue_wrap/` wraps the engine and holds no gameplay; `coop/` holds gameplay and reaches the
  engine only through `ue_wrap/`.
- Version-specific knowledge lives only in `ue_wrap/core/sdk_profile.h` (signatures, offsets)
  and `ue_wrap/core/sdk_profile_names.h` (content names).
- Any wire-format change bumps `kProtocolVersion` in `include/coop/net/protocol.h`.
- No game binaries or assets in git (`.gitignore`). No secrets in code.
- Never import a symbol from UE4SS (`.github/ci/abi_gate.py`).

## Central build workflow (user rule, 2026-11-06; full orchestration notes in `AGENTS.md`)

- Sub-agents **do not build**. They implement, self-review, commit on their branch, and report.
  A fresh worktree's first build is a full from-scratch compile; that was most of the waiting.
- The orchestrator merges each finished branch into `port/0.10.0` in the main checkout and runs
  ONE incremental build there (`build/votv-coop`, already warm), then the gates.
- If the build or a gate fails, the orchestrator sends the exact errors to the agent that wrote
  the change, waits for its fix commit, merges it and rebuilds.
- Only one build on the machine at a time, so it may use more of the throttled cores:
  `--parallel 1 -- /p:CL_MPCount=6` (the throttle still caps compilers at 8 of 16 cores, Idle).

## Build CPU budget (the machine crashed once from too many parallel builds)

- The box crashed **twice** (2026-11-06 13:47 and ~19:05). Build VERY low:
- At most **2 agents building at once**. Builds: `--parallel 1 -- /p:CL_MPCount=1`, Idle priority.
  Never start a second build in the same worktree while one runs.
- Compilers are capped at **8 of 16 cores (50%)** by `.claude/scripts/compiler-throttle.ps1`
  (main checkout, local-only). Before building, make sure it runs:
  `Get-CimInstance Win32_Process -Filter "Name='pwsh.exe'" | ? CommandLine -like '*compiler-throttle*'`;
  if empty, start it with
  `Start-Process pwsh -WindowStyle Hidden -ArgumentList '-NoProfile','-File','<main checkout>\.claude\scripts\compiler-throttle.ps1'`.

## Build quick ref (Windows, VS 2022)

```powershell
cmake -S src/votv-coop -B build/votv-coop -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static -DVCPKG_MANIFEST_MODE=ON
cmake --build build/votv-coop --config Release
```
