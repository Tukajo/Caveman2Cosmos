# Caveman2Cosmos Debugging & Performance Engineering Guide (`AGENTS.md`)

This repository is the Caveman2Cosmos (C2C) DLL/mod workspace. When debugging crashes, logic regressions, or turn-latency bottlenecks, follow the **Measure → Isolate → Verify → Deploy** workflow below.

---

## 1. Build, Deploy, and Cache-Clear Workflow (Linux / Wine / Proton)

- **Always build from the real path** (not the `/home/tjfrisch/Code/caveman2cosmos` symlink) so Wine/MSVC path case-sensitivity matches FASTBuild rules:
  ```bash
  cd /mnt/disk2/Code/Caveman2Cosmos/Sources && WINEDEBUG=-all wine ../Tools/FBuild.exe -summary -wait Release-build
  ```
- **Deploy DLL + PDB** to `Assets/`, **verify SHA-256 checksums** across `Build/Release/`, `Assets/`, and the Steam mod `Assets/` directory, and **clear the Proton BtS cache** before asking the user to reboot/re-test:
  ```bash
  REPO="/mnt/disk2/Code/Caveman2Cosmos"
  MOD="/mnt/disk2/SteamLibrary/steamapps/common/Sid Meier's Civilization IV Beyond the Sword/Beyond the Sword/Mods/Caveman2Cosmos"
  CACHE="/mnt/disk2/SteamLibrary/steamapps/compatdata/8800/pfx/drive_c/users/steamuser/AppData/Local/My Games/Beyond the Sword/cache"

  cp -f "$REPO/Build/Release/CvGameCoreDLL.dll" "$REPO/Assets/CvGameCoreDLL.dll"
  cp -f "$REPO/Build/Release/CvGameCoreDLL.pdb" "$REPO/Assets/CvGameCoreDLL.pdb"

  sha256sum \
    "$REPO/Build/Release/CvGameCoreDLL.dll" "$REPO/Assets/CvGameCoreDLL.dll" "$MOD/Assets/CvGameCoreDLL.dll" \
    "$REPO/Build/Release/CvGameCoreDLL.pdb" "$REPO/Assets/CvGameCoreDLL.pdb" "$MOD/Assets/CvGameCoreDLL.pdb"

  rm -rf "$CACHE"/*
  ```

---

## 2. Log Locations

- **DLL Turn & Path Timing / Benchmark Logs** (append-only, in BtS install directory,
  `/mnt/disk2/SteamLibrary/steamapps/common/Sid Meier's Civilization IV Beyond the Sword/Beyond the Sword/`):
  - `PlayerTurnTiming.log`, `PathCallerTiming.log`, `TurnBenchmark.log`, `TurnBenchmarkDetailed.log`
- **Engine / Python / BBAI Logs** (in Proton prefix):
  - `/mnt/disk2/SteamLibrary/steamapps/compatdata/8800/pfx/drive_c/users/steamuser/Documents/My Games/Beyond the Sword/Logs/` (`PythonErr.log`, `PythonDbg.log`, `C2C.log`, `BBAI.log`, `xml.log`, etc.)

### Log Hygiene: Clear/Backup Logs **Between** Debug Iterations

All of the above logs are append-only across runs, reloads, and replays, so previous
experiments pollute every later analysis. **Before each new benchmark/debug run** (game
process not running), move the timing logs out and truncate the engine-held logs so each
iteration starts with clean, single-session files:

```bash
BTS_DIR="/mnt/disk2/SteamLibrary/steamapps/common/Sid Meier's Civilization IV Beyond the Sword/Beyond the Sword"
GAME_LOGS="/mnt/disk2/SteamLibrary/steamapps/compatdata/8800/pfx/drive_c/users/steamuser/Documents/My Games/Beyond the Sword/Logs"
BACKUP="$HOME/bench_logs/$(date +%Y%m%d_%H%M%S)"
mkdir -p "$BACKUP"

# DLL timing/benchmark logs: game is exited between runs, so MOVE them into the backup dir
for f in PlayerTurnTiming.log PathCallerTiming.log TurnBenchmark.log TurnBenchmarkDetailed.log; do
    [ -f "$BTS_DIR/$f" ] && mv -f "$BTS_DIR/$f" "$BACKUP/$f"
done

# Engine/Python logs: the engine may reopen/handle these — copy then TRUNCATE in place, never delete
for f in PythonErr.log PythonDbg.log C2C.log BBAI.log init.log; do
    [ -f "$GAME_LOGS/$f" ] && { cp -f "$GAME_LOGS/$f" "$BACKUP/$f"; : > "$GAME_LOGS/$f"; }
done

# Optional retention: keep only the 10 most recent backup sessions
ls -1dt "$HOME"/bench_logs/*/ 2>/dev/null | tail -n +11 | xargs -r rm -rf
```

- Save the `$BACKUP` path alongside the benchmark result so the exact raw logs for that
  iteration stay reviewable (the harness's `/tmp/opencode/bench_<stamp>/` copies only its
  newly appended byte-diff).
- After this rotation, each run's log contains exactly one contiguous session, so
  `tail -n +1`/full-file analysis is safe; the session-splitting rules in Rule 2 remain
  a fallback for ad-hoc or interrupted runs.
- Never rotate logs **while** `Civ4BeyondSword.exe` is running (a stale second instance
  will silently re-open and re-create them; `pgrep -fa run_turn_benchmark` first).
- **Benchmark launch hangs before engine init = check the GPU first.** If a local
  inference server (e.g. `~/Strata/engine/strata --serve`) holds most VRAM and pins the
  GPU at 100%, Civ4's DXVK device init blocks forever (symptom: process alive at 0 CPU,
  1 Wine thread in `futex_wait`, empty `app.log`/`PythonDbg.log`, no timing logs).
  `nvidia-smi` before launching; free ≥4 GB VRAM. To kill a stuck run, remember Wine
  truncates process names to 15 chars (`Civ4BeyondSword`, no `.exe`) and `pkill -f` can
  match your own shell command line — prefer `wineserver -k` with
  `WINEPREFIX=.../compatdata/8800/pfx`.

---

## 3. Generic Performance Debugging Skill

### Rule 1: Measure First — Never Guess the Bottleneck
Before optimizing any suspected loop:
1. Check existing coarse buckets in `PlayerTurnTiming.log` (`SETACTIVE_MS`, `DANGER_MS`, `MOVES_MS`, `SETINACTIVE_MS`) and `PathCallerTiming.log` (`OUTER -> INNER`).
2. If a large bucket is opaque (e.g., `SETACTIVE_MS`, `DOTURN_CITIES_MS`, or uninstrumented `UNITMOVES_MS`), add lightweight RAII sub-timers first:
   - **Sub-timers (`PlayerTurnTiming.log`)**: Declare `addPlayer*Ms(int iPlayer, double dMs)` in [Sources/SubTimerTag.h](file:///home/tjfrisch/Code/caveman2cosmos/Sources/SubTimerTag.h), store/reset/print the accumulator in [Sources/CvPlayer.cpp](file:///home/tjfrisch/Code/caveman2cosmos/Sources/CvPlayer.cpp), and wrap target scopes with `SimpleTimerScope timer(getOwner(), addPlayer*Ms);`.
   - **Path caller tags (`PathCallerTiming.log`)**: `#include "PathCallerTag.h"` and add `PATH_CALLER_TAG();` at the top of any function calling `generatePath(...)` that currently shows up as `(untagged)`.

### Rule 2: Analyze Logs by Contiguous Session & AI-Only Filtering
- Timing logs are **append-only** and saves may be reloaded/replayed. Never blindly sum the whole file or tail N arbitrary lines without checking turn continuity — unless the Section 2 log-hygiene rotation was run first, in which case the files contain only the newest session.
- Split logs into contiguous sessions (detecting where new instrumentation fields appear or where `TURN` decreases).
- Filter `HUMAN=0` when measuring AI turn latency (human `DURATION_MS` includes idle UI thinking time).
- When comparing before/after an optimization, prefer **same-turn replay comparisons** (e.g., reloading and re-taking the exact same turns) so unit counts, city counts, and wars match.

### Rule 3: Preserve Determinism & Game Semantics
- **Sync RNG (`GC.getGame().getSorenRandNum`)**: Never skip, reorder, or add `SorenRand` calls without guarding the exact same conditions, or multiplayer/replays will desync.
- **Path reuse & side effects**: `CvSelectionGroup::generatePath` in the new path generator ignores `bReuse` and recomputes A*, writing into `getPathGenerator()->getLastPath()`. Before removing a duplicate `generatePath` call, verify nothing between the two calls overwrote `getLastPath()`, and verify `getPathEndTurnPlot()` is only read when a path was actually generated.
- **Precomputed XML indices**: Check `CvBuildingInfo` / `CvUnitInfo` / `CvPromotionLineInfo` for existing post-load indices (such as `getEnabledBuildingsList()`) before writing new full-table scans.

### Rule 4: Back Optimizations with Standalone Equivalence & Benchmark Tests
- When optimizing non-trivial scoring, pruning, caching, or indexing logic, add or extend a standalone zero-engine-dependency C++11 test suite under `Test/<SuiteName>/` (see [Test/CvSafetyPruning/](file:///home/tjfrisch/Code/caveman2cosmos/Test/CvSafetyPruning/README.md) and [Test/CvCityTurnPerf/](file:///home/tjfrisch/Code/caveman2cosmos/Test/CvCityTurnPerf/README.md)):
  - Use a deterministic fixed-seed PRNG (32-bit LCG) so failures reproduce identically.
  - Assert **exact semantic equivalence** (bit-identical scores/choices/checksums) against a transcribed pre-optimization baseline.
  - Include a `--quick` flag for fast interactive runs alongside full-scale stress sweeps.
  - Compile and run with warnings enabled before deploying:
    ```bash
    g++ -O2 -std=c++11 -Wall -Wextra -o /tmp/test_bin <TestFile>.cpp && /tmp/test_bin --quick && /tmp/test_bin
    ```

---

## 4. Linux / Wine / Proton Launch & Auto-Load Mechanics

Launching `Civ4BeyondSword.exe` directly under Proton (without the Steam GUI) has specific engine/DRM constraints verified on this machine:

1. **Steamworks DRM (`SteamAppId`):** `Civ4BeyondSword.exe` requires `SteamAppId=8800`, `SteamGameId=8800`, `STEAM_COMPAT_CLIENT_INSTALL_PATH=~/.steam/debian-installation`, and `STEAM_COMPAT_DATA_PATH=/mnt/disk2/SteamLibrary/steamapps/compatdata/8800`. Without `SteamAppId=8800`, the binary exits immediately with code `0`.
2. **Mod Selection:** Do **not** pass `mod=\...` on the command line (causes `"invalid mod directory, ignoring"`). Ensure `Mod = Mods\Caveman2Cosmos` is set in `CivilizationIV.ini` (`.../pfx/drive_c/users/steamuser/Documents/My Games/Beyond the Sword/CivilizationIV.ini`).
3. **Why `/fxsload=` CLI Arguments Stall at the Main Menu:** Inside `Civ4BeyondSword.exe`, the `/fxsload=` parser scans `GetCommandLineA()` and extracts only a double-quote-delimited (`"`) value. Passing an unquoted argument such as `/fxsload=C:\bench.CivBeyondSwordSave` through `subprocess` causes Wine to omit quotes in `GetCommandLineA()`, so the engine discards the save path and sits at the Main Menu (`DBG: Begin MenuManager` in `init.log`).
4. **Reliable Auto-Load via `CivilizationIV.ini` (`QuickStart = 1`):**
   - `Civ4BeyondSword.exe` only honors `GameType = spLoad` and `FileName = <path relative to BtS working dir>` after `MenuManager` initializes when `QuickStart = 1` is also set (with `QuickStart = 0`, it skips `FileName` and waits at the Main Menu).
   - **Case-insensitivity required:** C2C's [Assets/Python/DataStorage/ScreenResolution.py](file:///home/tjfrisch/Code/caveman2cosmos/Assets/Python/DataStorage/ScreenResolution.py#L16-L33) rewrites `CivilizationIV.ini` via Python's `ConfigParser`, which lowercases option names (`quickstart`, `gametype`, `filename`, `nointromovie`). Always match INI keys case-insensitively (`(?mi)`).
5. **SteamStub Packing Note:** `Civ4BeyondSword.exe` has a `.bind` SteamStub section that encrypts `.text` on disk and unpacks it at runtime; static disassembly of `.text` from disk shows encrypted bytes.

---

## 5. Autonomous Turn-Benchmark & Optimization Loop (Skill)

An opt-in headless/unattended benchmark loop is wired into [Assets/Python/Revolution/Gameready/AIAutoPlay.py](file:///home/tjfrisch/Code/caveman2cosmos/Assets/Python/Revolution/Gameready/AIAutoPlay.py#L106-L135) (`C2C_BENCHMARK_TURNS`) and [Tools/run_turn_benchmark.py](file:///home/tjfrisch/Code/caveman2cosmos/Tools/run_turn_benchmark.py):
- On load (`onGameLoad`), if `C2C_BENCHMARK_TURNS=<N>` is set in single-player, `AIAutoPlay` records `GAME.getGameTurn()` and calls `GAME.setAIAutoPlay(GAME.getActivePlayer(), N)`.
- When `N` turns complete (`onBeginPlayerTurn` with `GAME.getAIAutoPlay(iPlayerAct) == 0` and `iGameTurn >= iBenchmarkStartTurn + N`), it calls `os._exit(0)` cleanly.
- [Tools/run_turn_benchmark.py](file:///home/tjfrisch/Code/caveman2cosmos/Tools/run_turn_benchmark.py) optionally deploys `Build/Release/CvGameCoreDLL.{dll,pdb}` to `Assets/` and clears the Proton cache, stages the target `.CivBeyondSwordSave` to `bench.CivBeyondSwordSave` in the BtS working directory, patches `CivilizationIV.ini` (`QuickStart = 1`, `GameType = spLoad`, `FileName = bench.CivBeyondSwordSave`, `NoIntroMovie = 1`) with full restoration in `finally`, runs Proton, and diffs strictly the newly appended bytes of `PlayerTurnTiming.log` and `PathCallerTiming.log`.

Follow this closed loop for every performance or debugging iteration:

1. **Measure & Isolate Target:**
   - Run a pinned-save baseline (e.g., 2–3 turns) or inspect the latest benchmark summary.
   - If the top time bucket is opaque (e.g., `(untagged) -> (untagged)` in `PathCallerTiming.log`, or `UNITMOVES_MS` / `DOTURN_CITIES_MS`), add `SimpleTimerScope` or `PATH_CALLER_TAG()` instrumentation first before changing algorithm logic.
2. **Design & Standalone Unit Test:**
   - Formulate a targeted, determinism-preserving change (`SorenRand` call order, RNG counts, tie-breaking, and side effects like `getLastPath()` preserved).
   - Add or extend a standalone C++11 equivalence + stress benchmark test under `Test/<SuiteName>/`, compile with `-O2 -std=c++11 -Wall -Wextra`, and confirm both `--quick` and full suites print `RESULT: PASS`.
3. **Build Release DLL:**
   ```bash
   cd /mnt/disk2/Code/Caveman2Cosmos/Sources && WINEDEBUG=-all wine ../Tools/FBuild.exe -summary -wait Release-build
   ```
4. **Run Unattended Proton Benchmark (always pin `--save` for A/B comparisons):**
   First rotate the logs per **Section 2 “Log Hygiene”** so this iteration's files contain
   only this run (the harness byte-diff keeps working, but raw logs stay human-readable
   and free of prior-run pollution). Then:
   ```bash
   python3 /mnt/disk2/Code/Caveman2Cosmos/Tools/run_turn_benchmark.py --turns 3 --save "<pinned_save_path>"
   ```
   *(Omit `--save` to auto-boot the newest pinned baseline in `~/bench_saves/baseline_turn*.CivBeyondSwordSave`
   (currently `baseline_turn1086`), falling back to the newest engine autosave. Pass `--no-deploy` to re-run without re-copying `Build/Release` or clearing cache.)*
5. **Verify & Decide:**
   - Check `PythonErr.log` (and `init.log` / `BBAI.log` if anything abnormal happened) to confirm no errors.
   - Compare same-turn buckets (`DURATION_MS`, `SETACTIVE_MS`, `CITY_DOPROD_MS`, `MOVES_MS`, `PATH_MS`, `PATH_COUNT`, `REACHABLE_MS`, and top path callers) against the baseline.
   - Keep and commit (`code + standalone test`) only if semantics are preserved and metrics measurably improve; otherwise revert and test the next hypothesis.
6. **Repeat:** Re-rank the remaining bottlenecks from the updated benchmark output and iterate on the next highest-impact target.
