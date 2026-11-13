# CvCityTurnPerf standalone baseline & equivalence tests

Standalone C++ test and microbenchmark suite for `CvCity::doTurn()` and
`CvCityAI` (`DOTURN_CITIES_MS`), following the same zero-engine-dependency
pattern as `Test/CvSafetyPruning`.

## What it covers

Models C2C-scale datasets across three game stages (`Early-Game`: 60 cities,
`Mid-Game`: 250 cities, `Late-Game-C2C`: 600 cities, with 4,500 buildings and
350 promotion lines) and verifies **exact semantic equivalence** plus
wall-clock throughput between current linear-scan baselines and indexed/cached
candidates:

1. **AutoBuild scan** (`CvCity::doAutobuild`) — full `GC.getNumBuildingInfos()`
   reverse scan vs. pre-indexed `isAutoBuild()` list.
2. **Outbreaks & Afflictions scan** (`CvCity::doTurn`) — full
   `GC.getNumPromotionLineInfos()` scan vs. pre-indexed `isAffliction()` list.
3. **BestBuilding evaluation** (`CvCityAI::AI_bestBuilding`) — full building
   table scan vs. compact per-city constructible-buildings list.

## Build & run

Linux / MinGW:

```sh
g++ -O2 -std=c++11 -Wall -Wextra -o city_perf_tests CvCityTurnPerfTests.cpp
./city_perf_tests          # full sweeps
./city_perf_tests --quick  # shrunk sweeps for interactive use
```

MSVC:

```
cl /EHsc /O2 CvCityTurnPerfTests.cpp
CvCityTurnPerfTests.exe
```

Exit code `0` and `RESULT: PASS` on success.
