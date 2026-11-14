# CvReachableDestFallback standalone tests

Tests for the dest-valid fallback in `CvReachablePlotSet::enumerateReachablePlotsInternal`
(`Sources/CvReachablePlotSet.cpp`). They test two things that point in opposite directions, and
the second one is why the optimization was rejected.

## A. The decision equivalence (would-be sound)

`ContextFreeNewPathValidFunc` sets `*pbValidAsTerminus = true` **before** it evaluates the
reveal/danger/move-through predicates. So for a BFS edge (`isTerminus = false`,
`bMoveTerminationChecksOnly = false`) the flag is true whenever the domain and ZoC gates pass, and
the BFS decision `bValid || bValidAsTerminus` is already fixed — the fallback's return value cannot
change it. On the pinned baseline that covers 253,456 of 253,600 fallback calls per turn (99.9%).

If `NewPathDestValid` were pure, those calls could be skipped. Tests 1–3 verify that, under the
purity assumption, decisions, flags, reachable sets, step distances, insertion order, and the
retained `pathValidInternal` cache-write trace are all identical; test 2 shows the ordering is
load-bearing (setting the flag after the internal evaluation diverges).

## B. The assumption fails in the engine (test 4)

`NewPathDestValid` is **not** pure: it calls `CvPlayerAI::AI_getAnyPlotDanger`, which mutates the
shared plot-danger LRU cache (`plotDangerCache`, `iLastUseCount`, `plotDangerCacheReads`), and
`getActivePlayerSafeRangeCache`. Skipping a lookup changes which entries survive, so a later read of
a plot whose state changed in between returns a different value — first-writer-wins. Test 4 models a
bounded LRU and shows the two call orders diverge on later reads (3.17M divergences over 200,000
cases).

This was confirmed on the pinned save (`baseline_turn1086`, turns 1086–1088): with the skip in place
the per-turn `REACH_EDGES` matched the baseline exactly for the first 50 player-turn rows and then
diverged, and `PATH_COUNT` changed (24,940 vs 25,108.7 per turn). A pure speed-up that changes
which AI decisions see which cached values is not admissible under Rule 3, so the skip was reverted.

What remains is the instrumentation: `REACH_FB` (fallbacks that can still change the decision) and
`REACH_DEAD` (fallbacks whose result cannot change the decision, but must still be executed because
of the shared cache).

## Build & run

```sh
g++ -O2 -std=c++11 -Wall -Wextra -o dest_tests CvReachableDestFallbackTests.cpp
./dest_tests          # full sweeps
./dest_tests --quick  # shrunk sweeps for interactive use
```

Exit code 0 and `RESULT: PASS` on success. Randomness uses a fixed 32-bit LCG, so a failure
reproduces identically. If anyone re-introduces a skip here, test 4 is the check that must change
from "assumption fails" to "assumption holds".
