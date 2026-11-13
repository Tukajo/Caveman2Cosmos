#!/usr/bin/env python3
"""
Unattended C2C turn-latency benchmark runner for Linux / Proton.

1. Optionally deploys Build/Release/CvGameCoreDLL.{dll,pdb} to Assets/ and
   clears the Proton BtS cache.
2. Launches Civ4BeyondSword.exe via Proton directly into a savegame with
   C2C_BENCHMARK_TURNS=<N>, which triggers AIAutoPlay on load and exits cleanly
   once N full turns complete.
3. Parses strictly the newly appended bytes of PlayerTurnTiming.log and
   PathCallerTiming.log and prints a per-turn + average summary.
"""

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
from collections import defaultdict

REPO = "/mnt/disk2/Code/Caveman2Cosmos"
BTS_DIR = "/mnt/disk2/SteamLibrary/steamapps/common/Sid Meier's Civilization IV Beyond the Sword/Beyond the Sword"
MOD_DIR = f"{BTS_DIR}/Mods/Caveman2Cosmos"
PROTON_BIN = "/mnt/disk2/SteamLibrary/steamapps/common/Proton - Experimental/proton"
STEAM_COMPAT_CLIENT = os.path.expanduser("~/.steam/debian-installation")
COMPAT_DATA = "/mnt/disk2/SteamLibrary/steamapps/compatdata/8800"
CACHE_DIR = f"{COMPAT_DATA}/pfx/drive_c/users/steamuser/AppData/Local/My Games/Beyond the Sword/cache"
AUTOSAVE_DIR = f"{COMPAT_DATA}/pfx/drive_c/users/steamuser/Documents/My Games/Beyond the Sword/Saves/single/auto"

PLAYER_LOG = f"{BTS_DIR}/PlayerTurnTiming.log"
PATH_LOG = f"{BTS_DIR}/PathCallerTiming.log"


def find_latest_save():
    saves = glob.glob(f"{AUTOSAVE_DIR}/*.CivBeyondSwordSave")
    if not saves:
        raise FileNotFoundError(f"No autosaves found in {AUTOSAVE_DIR}")
    return max(saves, key=os.path.getmtime)


def deploy_and_clear_cache():
    for ext in ("dll", "pdb"):
        src = f"{REPO}/Build/Release/CvGameCoreDLL.{ext}"
        dst = f"{REPO}/Assets/CvGameCoreDLL.{ext}"
        shutil.copy2(src, dst)
    if os.path.isdir(CACHE_DIR):
        for entry in os.listdir(CACHE_DIR):
            p = os.path.join(CACHE_DIR, entry)
            if os.path.isdir(p):
                shutil.rmtree(p, ignore_errors=True)
            else:
                os.remove(p)


def file_size(path):
    return os.path.getsize(path) if os.path.exists(path) else 0


def read_appended(path, offset):
    if not os.path.exists(path):
        return ""
    with open(path, "r", errors="replace") as f:
        f.seek(offset)
        return f.read()


def summarize_logs(player_text, path_text):
    fields = [
        "DURATION_MS", "SETACTIVE_MS", "DOTURN_MS", "DOTURN_CITIES_MS",
        "CITY_DOPROD_MS", "CITY_AIDO_MS", "CITY_AUTOBUILD_MS",
        "MOVES_MS", "UNITMOVES_MS", "PATH_MS", "PATH_COUNT",
        "REACHABLE_MS", "PATROL_MS", "SETINACTIVE_MS",
    ]
    by_turn = defaultdict(lambda: defaultdict(int))
    cnt_turn = defaultdict(int)

    for line in player_text.splitlines():
        tm = re.search(r"TURN (\d+) PLAYER (\d+)", line)
        if not tm:
            continue
        t = int(tm.group(1))
        hm = re.search(r"HUMAN=(\d+)", line)
        if hm and int(hm.group(1)) == 1:
            continue
        cnt_turn[t] += 1
        for fld in fields:
            m = re.search(rf"\b{fld}=(\d+)", line)
            if m:
                by_turn[t][fld] += int(m.group(1))

    turns = sorted(by_turn.keys())
    if not turns:
        print("ERROR: No new AI turn entries recorded in PlayerTurnTiming.log")
        return 1

    print(f"\n=== Benchmark Results ({len(turns)} turns: {turns[0]}..{turns[-1]}) ===")
    totals = defaultdict(int)
    for t in turns:
        d = by_turn[t]
        for k, v in d.items():
            totals[k] += v
        print(
            f"  Turn {t} ({cnt_turn[t]} AI): DUR={d['DURATION_MS']:5d}ms | "
            f"SETACT={d['SETACTIVE_MS']:4d}ms (CITIES={d['DOTURN_CITIES_MS']:4d}, DOPROD={d['CITY_DOPROD_MS']:4d}) | "
            f"MOVES={d['MOVES_MS']:4d}ms (PATH={d['PATH_MS']:4d}ms/{d['PATH_COUNT']:5d}cnt, REACH={d['REACHABLE_MS']:3d}ms)"
        )

    n = len(turns)
    print(f"\n--- Averages per turn ({n} turns) ---")
    for fld in fields:
        print(f"  {fld:18s}: {totals[fld] / n:8.1f}")

    by_pair = defaultdict(lambda: [0.0, 0])
    for line in path_text.splitlines():
        pm = re.search(r"PATH_MS=([\d.]+)", line)
        cm = re.search(r"PATH_COUNT=(\d+)", line)
        om = re.search(r"OUTER=(\S+)", line)
        im = re.search(r"INNER=(\S+)", line)
        if pm and cm and om and im:
            by_pair[(om.group(1), im.group(1))][0] += float(pm.group(1))
            by_pair[(om.group(1), im.group(1))][1] += int(cm.group(1))

    tot_pms = sum(v[0] for v in by_pair.values())
    if tot_pms > 0:
        print(f"\n--- Top 10 Path Callers (total {tot_pms:.1f} ms across {n} turns) ---")
        for (o, i), (ms, cnt) in sorted(by_pair.items(), key=lambda x: -x[1][0])[:10]:
            print(f"  {ms:7.1f} ms ({100.0 * ms / tot_pms:4.1f}%) | {cnt:6d} calls | {o} -> {i}")
    return 0


def main():
    parser = argparse.ArgumentParser(description="Run unattended C2C turn benchmark via Proton.")
    parser.add_argument("--save", default=None, help="Path to .CivBeyondSwordSave (default: latest autosave)")
    parser.add_argument("--turns", type=int, default=3, help="Number of AIAutoPlay turns to run (default: 3)")
    parser.add_argument("--timeout", type=int, default=300, help="Timeout in seconds (default: 300)")
    parser.add_argument("--no-deploy", action="store_true", help="Skip copying Build/Release DLL/PDB and clearing cache")
    args = parser.parse_args()

    save_path = args.save or find_latest_save()
    if not os.path.exists(save_path):
        print(f"Save not found: {save_path}", file=sys.stderr)
        return 1

    if not args.no_deploy:
        print("Deploying Build/Release DLL+PDB and clearing BtS cache...")
        deploy_and_clear_cache()

    p_off = file_size(PLAYER_LOG)
    c_off = file_size(PATH_LOG)

    # Copy save into BTS_DIR/bench.CivBeyondSwordSave ("Save Path - relative to
    # working (Civ4) folder") and set QuickStart=1, GameType=spLoad,
    # FileName=bench.CivBeyondSwordSave in CivilizationIV.ini (restored in finally).
    bench_save_path = f"{BTS_DIR}/bench.CivBeyondSwordSave"
    shutil.copy2(save_path, bench_save_path)

    ini_path = f"{COMPAT_DATA}/pfx/drive_c/users/steamuser/Documents/My Games/Beyond the Sword/CivilizationIV.ini"
    with open(ini_path, "r", errors="replace") as f:
        orig_ini = f.read()

    mod_ini = re.sub(r"(?mi)^quickstart\s*=.*$", "QuickStart = 1", orig_ini)
    mod_ini = re.sub(r"(?mi)^gametype\s*=.*$", "GameType = spLoad", mod_ini)
    mod_ini = re.sub(r"(?mi)^filename\s*=.*$", "FileName = bench.CivBeyondSwordSave", mod_ini)
    mod_ini = re.sub(r"(?mi)^nointromovie\s*=.*$", "NoIntroMovie = 1", mod_ini)

    exe = f"{BTS_DIR}/Civ4BeyondSword.exe"
    cmd = [PROTON_BIN, "run", exe]

    env = os.environ.copy()
    env["STEAM_COMPAT_CLIENT_INSTALL_PATH"] = STEAM_COMPAT_CLIENT
    env["STEAM_COMPAT_DATA_PATH"] = COMPAT_DATA
    env["SteamAppId"] = "8800"
    env["SteamGameId"] = "8800"
    env["C2C_BENCHMARK_TURNS"] = str(args.turns)

    print(f"Launching BtS for {args.turns} turns on save: {os.path.basename(save_path)}")
    try:
        with open(ini_path, "w") as f:
            f.write(mod_ini)
        proc = subprocess.run(cmd, env=env, cwd=BTS_DIR, timeout=args.timeout)
        print(f"BtS exited with code {proc.returncode}")
    except subprocess.TimeoutExpired:
        subprocess.run(["killall", "-9", "Civ4BeyondSword.exe"], stderr=subprocess.DEVNULL)
        print(f"ERROR: Benchmark timed out after {args.timeout}s", file=sys.stderr)
        return 1
    finally:
        with open(ini_path, "w") as f:
            f.write(orig_ini)
        if os.path.exists(bench_save_path):
            os.remove(bench_save_path)

    player_new = read_appended(PLAYER_LOG, p_off)
    path_new = read_appended(PATH_LOG, c_off)
    return summarize_logs(player_new, path_new)


if __name__ == "__main__":
    sys.exit(main())
