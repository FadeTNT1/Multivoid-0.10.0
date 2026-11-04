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
- Every sub-agent / delegated child must invoke the `caveman:caveman` skill (full) first thing,
  and its brief must say so. Code, commits and docs stay normal prose.
- Explore with GPT-6.1 Sol or a read-only locator before reading many files yourself; bring back
  conclusions (file:line), not file dumps.
- Delegate with a tight brief: goal, files, what is already ruled out, done-criteria.
- One authority per task: do not run the same investigation twice in parallel.
- A child never commits to `main`; work in a worktree/branch and report what changed.

## Hard rules (from CONTRIBUTING.md, summarised)

- `ue_wrap/` wraps the engine and holds no gameplay; `coop/` holds gameplay and reaches the
  engine only through `ue_wrap/`.
- Version-specific knowledge lives only in `ue_wrap/core/sdk_profile.h` (signatures, offsets)
  and `ue_wrap/core/sdk_profile_names.h` (content names).
- Any wire-format change bumps `kProtocolVersion` in `include/coop/net/protocol.h`.
- No game binaries or assets in git (`.gitignore`). No secrets in code.
- Never import a symbol from UE4SS (`.github/ci/abi_gate.py`).

## Build quick ref (Windows, VS 2022)

```powershell
cmake -S src/votv-coop -B build/votv-coop -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static -DVCPKG_MANIFEST_MODE=ON
cmake --build build/votv-coop --config Release
```
