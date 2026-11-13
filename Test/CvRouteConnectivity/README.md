# CvRouteConnectivity — RouteFinder component-flood equivalence tests

Standalone, zero-engine-dependency C++11 suite for the `CvCityAI::AI_updateRouteToCity`
optimization (worker `AI_routeCity` hotspot: one UNITAI_WORKER doUnitAIMove call
burned ~3.7-4.8 s/turn on the pinned benchmark save, `WSECT_MS=rtCity:...`).

## What is optimized
Original: for every other same-area team city B, a fresh `GC.getRouteFinder()`
FAStar `GeneratePath(A -> B)` runs (`routeValid`: entered plot is passable iff
`(unowned OR team == player team) AND plot.routeType == player.getBestRoute(plot)`,
start node always valid). One disconnected pair forces every city to redo all
pairwise searches -> ~n^2 full-map FAStar explorations per turn, invisible to
PATH_MS (separate finder).

Optimized: the validity predicate is per-node, so reachability is an
8-connected component relation. Flood components once per (player, route-epoch),
then answer "is B reachable from A?" in O(1)+8: `B true && ((A true &&
comp(A)==comp(B)) || some true neighbor N of A has comp(N)==comp(B))`.

## What is verified
- `randCase`: random grids (20-60 x 20-60, wrapX sometimes, mixed ownership,
  random route/bestRoute patches, 12 city centers) — old pairwise-BFS model vs
  new flood-comp model must select the identical pBestCity (or none) for every
  city, preserving loop order and strict-`<` plotDistance tie-breaking.
- 3000 seeds x 12 city checks = 36 000 selections, 0 mismatches => PASS.
- Benchmark: fully routed 300x200 map cut by barriers (the in-game pathology:
  huge components + failed searches) — old model ~43 ms vs flood ~0.7 ms (~60x).

Note: the earlier 8-neighborhood bug (dy[5] typo making adjacency asymmetric)
was caught by exactly these equivalence checks — adjacency arrays must stay
symmetric for component semantics to match pairwise search.

## Build & run
```bash
g++ -O2 -std=c++11 -Wall -Wextra -o /tmp/opencode/route_conn_tests CvRouteConnectivityTests.cpp
/tmp/opencode/route_conn_tests --quick && /tmp/opencode/route_conn_tests
```
