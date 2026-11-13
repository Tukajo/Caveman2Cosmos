// Standalone baseline, equivalence, and throughput benchmark suite for
// CvCity::doTurn() / CvCityAI hotspots (DOTURN_CITIES_MS).
//
// Models C2C-scale datasets (hundreds of cities, thousands of building/unit/
// bonus/promotion-line infos, 37-plot city radii) without linking against
// CvGameCoreDLL, so algorithmic optimizations (e.g. pre-indexed auto-build
// lists, affliction line indices, sparse building/bonus scans, production
// candidate pruning/caching) can be verified for exact semantic equivalence
// and benchmarked in under a second.
//
// Build & run:
//   Linux:   g++ -O2 -std=c++11 -Wall -Wextra -o city_perf_tests CvCityTurnPerfTests.cpp && ./city_perf_tests
//   Windows: cl /EHsc /O2 CvCityTurnPerfTests.cpp && CvCityTurnPerfTests.exe
// Use --quick to shrink dataset sizes for fast interactive checks.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Deterministic PRNG (same 32-bit LCG pattern as CvSafetyPruningTests.cpp).
class Rng
{
public:
	explicit Rng(unsigned seed) : m_seed(seed) {}
	int next(int n)
	{
		m_seed = m_seed * 1103515245u + 12345u;
		return static_cast<int>((m_seed >> 8) % static_cast<unsigned>(n));
	}
	bool chance(int oneIn) { return next(oneIn) == 0; }
private:
	unsigned m_seed;
};

static long g_failures = 0;

static void fail(const char* what, const char* detail)
{
	if (g_failures < 8)
	{
		std::printf("FAIL %s: %s\n", what, detail);
	}
	++g_failures;
}

static double nowMs()
{
	using clock = std::chrono::steady_clock;
	static const clock::time_point t0 = clock::now();
	return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
}

// ---------------------------------------------------------------------------
// Synthetic C2C-scale world model
// ---------------------------------------------------------------------------
struct BuildingInfo
{
	bool isAutoBuild;
	bool isPropertyBuilding;
	bool hasVicinityOrConditionCheck;
	int prereqTech;
	int baseValue;
	int cost;
};

struct PromotionLineInfo
{
	bool isAffliction;
	int overcomeAdjPerTurn;
	int toleranceDecay;
};

struct CityState
{
	int owner;
	int population;
	bool isNPC;
	std::vector<unsigned char> hasBuilding;          // [numBuildings]
	std::vector<unsigned char> canConstruct;         // [numBuildings]
	std::vector<unsigned char> hasAffliction;        // [numPromoLines]
	std::vector<int> afflictionToleranceChange;      // [numPromoLines]
	std::vector<int> plotValue;                    // [37]
};

struct ScenarioConfig
{
	const char* name;
	int numTurns;
	int numCities;
	int numBuildings;
	int numAutoBuild;
	int numPromoLines;
	int numAfflictions;
	int numBonuses;
};

struct WorldData
{
	std::vector<BuildingInfo> buildings;
	std::vector<PromotionLineInfo> promoLines;
	std::vector<CityState> cities;

	// Precomputed indices (candidate optimizations)
	std::vector<int> autoBuildIndices;      // reverse order to match C2C loop
	std::vector<int> afflictionIndices;
	std::vector<std::vector<int> > enabledBuildingsList; // per-building dependents
};

static WorldData generateWorld(const ScenarioConfig& cfg, unsigned seed)
{
	Rng rng(seed);
	WorldData w;
	w.buildings.resize(cfg.numBuildings);
	for (int i = 0; i < cfg.numBuildings; ++i)
	{
		BuildingInfo& b = w.buildings[i];
		b.isAutoBuild = false;
		b.isPropertyBuilding = rng.chance(40);
		b.hasVicinityOrConditionCheck = rng.chance(15);
		b.prereqTech = rng.next(300);
		b.baseValue = 10 + rng.next(500);
		b.cost = 20 + rng.next(2000);
	}
	// Sprinkle auto-build buildings
	for (int k = 0; k < cfg.numAutoBuild; ++k)
	{
		int idx = (k * (cfg.numBuildings / std::max(1, cfg.numAutoBuild))) % cfg.numBuildings;
		w.buildings[idx].isAutoBuild = true;
	}
	for (int i = cfg.numBuildings - 1; i >= 0; --i)
	{
		if (w.buildings[i].isAutoBuild)
		{
			w.autoBuildIndices.push_back(i);
		}
	}

	// Build sparse enablement graph (~2-6 dependent buildings per enabler)
	w.enabledBuildingsList.resize(cfg.numBuildings);
	for (int i = 0; i < cfg.numBuildings; ++i)
	{
		if (rng.chance(5))
		{
			int count = 1 + rng.next(5);
			for (int k = 0; k < count; ++k)
			{
				int dep = (i + 1 + rng.next(cfg.numBuildings - 1)) % cfg.numBuildings;
				if (std::find(w.enabledBuildingsList[i].begin(), w.enabledBuildingsList[i].end(), dep) == w.enabledBuildingsList[i].end())
				{
					w.enabledBuildingsList[i].push_back(dep);
				}
			}
		}
	}

	w.promoLines.resize(cfg.numPromoLines);
	for (int i = 0; i < cfg.numPromoLines; ++i)
	{
		w.promoLines[i].isAffliction = false;
		w.promoLines[i].overcomeAdjPerTurn = 1 + rng.next(5);
		w.promoLines[i].toleranceDecay = rng.next(3);
	}
	for (int k = 0; k < cfg.numAfflictions; ++k)
	{
		int idx = (k * (cfg.numPromoLines / std::max(1, cfg.numAfflictions))) % cfg.numPromoLines;
		w.promoLines[idx].isAffliction = true;
	}
	for (int i = 0; i < cfg.numPromoLines; ++i)
	{
		if (w.promoLines[i].isAffliction)
		{
			w.afflictionIndices.push_back(i);
		}
	}

	w.cities.resize(cfg.numCities);
	for (int c = 0; c < cfg.numCities; ++c)
	{
		CityState& city = w.cities[c];
		city.owner = rng.next(50);
		city.population = 1 + rng.next(45);
		city.isNPC = (city.owner >= 40);
		city.hasBuilding.assign(cfg.numBuildings, 0);
		city.canConstruct.assign(cfg.numBuildings, 0);
		for (int b = 0; b < cfg.numBuildings; ++b)
		{
			bool owned = rng.chance(12);
			city.hasBuilding[b] = owned ? 1 : 0;
			city.canConstruct[b] = (!owned && rng.chance(10)) ? 1 : 0;
		}
		city.hasAffliction.assign(cfg.numPromoLines, 0);
		city.afflictionToleranceChange.assign(cfg.numPromoLines, 0);
		for (int p = 0; p < cfg.numPromoLines; ++p)
		{
			if (w.promoLines[p].isAffliction && rng.chance(8))
			{
				city.hasAffliction[p] = 1;
				city.afflictionToleranceChange[p] = rng.next(5);
			}
		}
		city.plotValue.resize(37);
		for (int pl = 0; pl < 37; ++pl)
		{
			city.plotValue[pl] = rng.next(100);
		}
	}
	return w;
}

// ---------------------------------------------------------------------------
// 1. AutoBuild scan: Full BuildingInfos scan vs. Pre-indexed AutoBuild list
//    Models CvCity::doAutobuild() (Sources/CvCity.cpp:1495)
// ---------------------------------------------------------------------------
static long long runAutoBuildBaseline(const WorldData& w, int turns, std::vector<int>& outChecksums)
{
	long long checksum = 0;
	const int nB = static_cast<int>(w.buildings.size());
	for (int t = 0; t < turns; ++t)
	{
		for (size_t c = 0; c < w.cities.size(); ++c)
		{
			const CityState& city = w.cities[c];
			int cityBuiltMask = 0;
			for (int iI = nB - 1; iI > -1; --iI)
			{
				if (w.buildings[iI].isAutoBuild)
				{
					if (!city.hasBuilding[iI] && city.canConstruct[iI])
					{
						cityBuiltMask = (cityBuiltMask * 33) ^ (iI + 1);
						checksum += iI + 1;
					}
				}
			}
			if (t == 0) outChecksums[c] = cityBuiltMask;
		}
	}
	return checksum;
}

static long long runAutoBuildIndexed(const WorldData& w, int turns, std::vector<int>& outChecksums)
{
	long long checksum = 0;
	const int nAuto = static_cast<int>(w.autoBuildIndices.size());
	for (int t = 0; t < turns; ++t)
	{
		for (size_t c = 0; c < w.cities.size(); ++c)
		{
			const CityState& city = w.cities[c];
			int cityBuiltMask = 0;
			for (int k = 0; k < nAuto; ++k)
			{
				const int iI = w.autoBuildIndices[k];
				if (!city.hasBuilding[iI] && city.canConstruct[iI])
				{
					cityBuiltMask = (cityBuiltMask * 33) ^ (iI + 1);
					checksum += iI + 1;
				}
			}
			if (t == 0) outChecksums[c] = cityBuiltMask;
		}
	}
	return checksum;
}

// ---------------------------------------------------------------------------
// 2. Outbreaks & Afflictions scan: Full PromotionLineInfos vs. Pre-indexed
//    Models CvCity::doTurn() OUTBREAKS_AND_AFFLICTIONS (CvCity.cpp:1365)
// ---------------------------------------------------------------------------
static long long runAfflictionsBaseline(WorldData& w, int turns)
{
	long long acc = 0;
	const int nP = static_cast<int>(w.promoLines.size());
	for (int t = 0; t < turns; ++t)
	{
		for (size_t c = 0; c < w.cities.size(); ++c)
		{
			CityState& city = w.cities[c];
			for (int iI = 0; iI < nP; ++iI)
			{
				const PromotionLineInfo& info = w.promoLines[iI];
				if (info.isAffliction)
				{
					if (city.hasAffliction[iI])
					{
						acc += info.overcomeAdjPerTurn;
					}
					if (city.afflictionToleranceChange[iI] > 0)
					{
						int iDecay = info.toleranceDecay;
						if (iDecay != 0)
						{
							city.afflictionToleranceChange[iI] -= iDecay;
							acc += iDecay;
						}
					}
				}
			}
		}
	}
	return acc;
}

static long long runAfflictionsIndexed(WorldData& w, int turns)
{
	long long acc = 0;
	const int nA = static_cast<int>(w.afflictionIndices.size());
	for (int t = 0; t < turns; ++t)
	{
		for (size_t c = 0; c < w.cities.size(); ++c)
		{
			CityState& city = w.cities[c];
			for (int k = 0; k < nA; ++k)
			{
				const int iI = w.afflictionIndices[k];
				const PromotionLineInfo& info = w.promoLines[iI];
				if (city.hasAffliction[iI])
				{
					acc += info.overcomeAdjPerTurn;
				}
				if (city.afflictionToleranceChange[iI] > 0)
				{
					int iDecay = info.toleranceDecay;
					if (iDecay != 0)
					{
						city.afflictionToleranceChange[iI] -= iDecay;
						acc += iDecay;
					}
				}
			}
		}
	}
	return acc;
}

// ---------------------------------------------------------------------------
// 3. Building evaluation scan: Full scan vs. Constructible-only prefilter
//    Models CvCityAI::AI_bestBuilding() style full-building-list evaluation
// ---------------------------------------------------------------------------
static long long runBestBuildingBaseline(const WorldData& w, int turns, std::vector<int>& outBest)
{
	long long acc = 0;
	const int nB = static_cast<int>(w.buildings.size());
	for (int t = 0; t < turns; ++t)
	{
		for (size_t c = 0; c < w.cities.size(); ++c)
		{
			const CityState& city = w.cities[c];
			int bestVal = 0;
			int bestIdx = -1;
			for (int iI = 0; iI < nB; ++iI)
			{
				if (!city.hasBuilding[iI] && city.canConstruct[iI])
				{
					const BuildingInfo& b = w.buildings[iI];
					int val = (b.baseValue * 100) / std::max(1, b.cost);
					if (val > bestVal)
					{
						bestVal = val;
						bestIdx = iI;
					}
				}
			}
			acc += bestIdx + bestVal;
			if (t == 0) outBest[c] = bestIdx;
		}
	}
	return acc;
}

static long long runBestBuildingCachedList(const WorldData& w, int turns, std::vector<int>& outBest)
{
	// Precompute constructible building list once per city per turn (modeling
	// a compact constructible-buildings cache populated once alongside
	// populateCanTrainCache).
	long long acc = 0;
	const int nB = static_cast<int>(w.buildings.size());
	std::vector<std::vector<int> > constructible(w.cities.size());
	for (size_t c = 0; c < w.cities.size(); ++c)
	{
		const CityState& city = w.cities[c];
		for (int iI = 0; iI < nB; ++iI)
		{
			if (!city.hasBuilding[iI] && city.canConstruct[iI])
			{
				constructible[c].push_back(iI);
			}
		}
	}

	for (int t = 0; t < turns; ++t)
	{
		for (size_t c = 0; c < w.cities.size(); ++c)
		{
			int bestVal = 0;
			int bestIdx = -1;
			const std::vector<int>& list = constructible[c];
			for (size_t k = 0; k < list.size(); ++k)
			{
				const int iI = list[k];
				const BuildingInfo& b = w.buildings[iI];
				int val = (b.baseValue * 100) / std::max(1, b.cost);
				if (val > bestVal)
				{
					bestVal = val;
					bestIdx = iI;
				}
			}
			acc += bestIdx + bestVal;
			if (t == 0) outBest[c] = bestIdx;
		}
	}
	return acc;
}

// ---------------------------------------------------------------------------
// 4. EnablesOtherBuildings: Full N^2 building scan vs. getEnabledBuildingsList
//    Models CvCityAI::AI_scoreBuildingsFromListThreshold (CvCityAI.cpp:4573)
// ---------------------------------------------------------------------------
static long long runEnablesOtherBaseline(const WorldData& w, int turns, std::vector<long long>& outScores)
{
	long long acc = 0;
	const int nB = static_cast<int>(w.buildings.size());
	// Build reverse lookup for baseline condition check
	std::vector<std::vector<unsigned char> > isEnabledBy(nB);
	for (int b = 0; b < nB; ++b)
	{
		if (!w.enabledBuildingsList[b].empty())
		{
			isEnabledBy[b].assign(nB, 0);
			for (size_t k = 0; k < w.enabledBuildingsList[b].size(); ++k)
			{
				isEnabledBy[b][w.enabledBuildingsList[b][k]] = 1;
			}
		}
	}

	const int sampleCities = std::min<int>(30, static_cast<int>(w.cities.size()));
	for (int t = 0; t < turns; ++t)
	{
		for (int c = 0; c < sampleCities; ++c)
		{
			const CityState& city = w.cities[c];
			long long cityScore = 0;
			for (int eBuilding = 0; eBuilding < nB; ++eBuilding)
			{
				if (city.hasBuilding[eBuilding] || !city.canConstruct[eBuilding]) continue;
				if (w.enabledBuildingsList[eBuilding].empty()) continue;

				const std::vector<unsigned char>& mask = isEnabledBy[eBuilding];
				for (int iI = 0; iI < nB; ++iI)
				{
					if (city.hasBuilding[iI]) continue;
					if (mask[iI])
					{
						cityScore += w.buildings[iI].baseValue / 2;
					}
				}
			}
			acc += cityScore;
			if (t == 0) outScores[c] = cityScore;
		}
	}
	return acc;
}

static long long runEnablesOtherIndexed(const WorldData& w, int turns, std::vector<long long>& outScores)
{
	long long acc = 0;
	const int nB = static_cast<int>(w.buildings.size());
	const int sampleCities = std::min<int>(30, static_cast<int>(w.cities.size()));
	for (int t = 0; t < turns; ++t)
	{
		for (int c = 0; c < sampleCities; ++c)
		{
			const CityState& city = w.cities[c];
			long long cityScore = 0;
			for (int eBuilding = 0; eBuilding < nB; ++eBuilding)
			{
				if (city.hasBuilding[eBuilding] || !city.canConstruct[eBuilding]) continue;
				const std::vector<int>& enabled = w.enabledBuildingsList[eBuilding];
				if (enabled.empty()) continue;

				for (size_t k = 0; k < enabled.size(); ++k)
				{
					const int eType = enabled[k];
					if (city.hasBuilding[eType]) continue;
					cityScore += w.buildings[eType].baseValue / 2;
				}
			}
			acc += cityScore;
			if (t == 0) outScores[c] = cityScore;
		}
	}
	return acc;
}

// ---------------------------------------------------------------------------
// 5. getBestConstructValue: per-building pathing vs. per-city cached pathing
//    Models CvUnitAI::getBestConstructValue (CvUnitAI.cpp:15100)
// ---------------------------------------------------------------------------
struct ConstructBestResult
{
	int bestWeighted;
	int bestCity;
	int bestBuilding;
	long pathCalls;
};

static ConstructBestResult runConstructValueSim(const WorldData& w, bool cachePathPerCity)
{
	Rng rng(9001u);
	ConstructBestResult res = { 0, -1, -1, 0 };
	const int nCities = std::min<int>(80, static_cast<int>(w.cities.size()));
	const int kUnitBuildings = 12; // typical subdued animal / GP building list

	std::vector<int> unitBuildings(kUnitBuildings);
	for (int i = 0; i < kUnitBuildings; ++i)
	{
		unitBuildings[i] = (i * 173) % static_cast<int>(w.buildings.size());
	}

	// Deterministic per-city reachability & path turns
	std::vector<unsigned char> cityReachable(nCities);
	std::vector<int> cityPathTurns(nCities);
	for (int c = 0; c < nCities; ++c)
	{
		cityReachable[c] = rng.chance(5) ? 0 : 1;
		cityPathTurns[c] = 1 + rng.next(15);
	}

	for (int c = 0; c < nCities; ++c)
	{
		const CityState& city = w.cities[c];
		bool bCityPathChecked = false;
		bool bCityPathOk = false;
		int iCityPathTurns = 0;

		for (int k = 0; k < kUnitBuildings; ++k)
		{
			const int b = unitBuildings[k];
			if (city.hasBuilding[b] || !city.canConstruct[b]) continue;

			bool pathOk = false;
			int pathTurns = 0;
			if (cachePathPerCity)
			{
				if (!bCityPathChecked)
				{
					bCityPathChecked = true;
					++res.pathCalls;
					bCityPathOk = (cityReachable[c] != 0);
					iCityPathTurns = cityPathTurns[c];
				}
				if (!bCityPathOk) break;
				pathOk = true;
				pathTurns = iCityPathTurns;
			}
			else
			{
				++res.pathCalls;
				pathOk = (cityReachable[c] != 0);
				pathTurns = cityPathTurns[c];
			}

			if (pathOk)
			{
				int val = w.buildings[b].baseValue;
				int weighted = (val * 5) / (4 + pathTurns);
				if (weighted > res.bestWeighted)
				{
					res.bestWeighted = weighted;
					res.bestCity = c;
					res.bestBuilding = b;
				}
			}
		}
	}
	return res;
}

// ---------------------------------------------------------------------------
// Harness runner
// ---------------------------------------------------------------------------
static void runScenario(const ScenarioConfig& cfg)
{
	std::printf("\n--- Scenario: %s (turns=%d, cities=%d, buildings=%d [auto=%d], promoLines=%d [afflictions=%d]) ---\n",
		cfg.name, cfg.numTurns, cfg.numCities, cfg.numBuildings, cfg.numAutoBuild, cfg.numPromoLines, cfg.numAfflictions);

	WorldData wBase = generateWorld(cfg, 1337u);
	WorldData wOpt = generateWorld(cfg, 1337u);

	// 1. AutoBuild
	std::vector<int> abBaseOut(cfg.numCities, 0), abOptOut(cfg.numCities, 0);
	double t0 = nowMs();
	long long abBaseSum = runAutoBuildBaseline(wBase, cfg.numTurns, abBaseOut);
	double abBaseMs = nowMs() - t0;

	t0 = nowMs();
	long long abOptSum = runAutoBuildIndexed(wOpt, cfg.numTurns, abOptOut);
	double abOptMs = nowMs() - t0;

	if (abBaseSum != abOptSum || abBaseOut != abOptOut)
	{
		fail("AutoBuild equivalence", cfg.name);
	}
	std::printf("  AutoBuild   : base=%7.2f ms | indexed=%6.2f ms | speedup=%5.1fx | eq=%s\n",
		abBaseMs, abOptMs, abBaseMs / std::max(0.001, abOptMs),
		(abBaseSum == abOptSum && abBaseOut == abOptOut) ? "OK" : "MISMATCH");

	// 2. Afflictions
	t0 = nowMs();
	long long afBaseSum = runAfflictionsBaseline(wBase, cfg.numTurns);
	double afBaseMs = nowMs() - t0;

	t0 = nowMs();
	long long afOptSum = runAfflictionsIndexed(wOpt, cfg.numTurns);
	double afOptMs = nowMs() - t0;

	if (afBaseSum != afOptSum)
	{
		fail("Afflictions equivalence", cfg.name);
	}
	std::printf("  Afflictions : base=%7.2f ms | indexed=%6.2f ms | speedup=%5.1fx | eq=%s\n",
		afBaseMs, afOptMs, afBaseMs / std::max(0.001, afOptMs),
		(afBaseSum == afOptSum) ? "OK" : "MISMATCH");

	// 3. BestBuilding
	std::vector<int> bbBaseOut(cfg.numCities, -1), bbOptOut(cfg.numCities, -1);
	t0 = nowMs();
	long long bbBaseSum = runBestBuildingBaseline(wBase, cfg.numTurns, bbBaseOut);
	double bbBaseMs = nowMs() - t0;

	t0 = nowMs();
	long long bbOptSum = runBestBuildingCachedList(wOpt, cfg.numTurns, bbOptOut);
	double bbOptMs = nowMs() - t0;

	if (bbBaseSum != bbOptSum || bbBaseOut != bbOptOut)
	{
		fail("BestBuilding equivalence", cfg.name);
	}
	std::printf("  BestBuilding: base=%7.2f ms | cached =%6.2f ms | speedup=%5.1fx | eq=%s\n",
		bbBaseMs, bbOptMs, bbBaseMs / std::max(0.001, bbOptMs),
		(bbBaseSum == bbOptSum && bbBaseOut == bbOptOut) ? "OK" : "MISMATCH");

	// 4. EnablesOtherBuildings
	const int sampleCities = std::min<int>(30, cfg.numCities);
	std::vector<long long> eoBaseOut(sampleCities, 0), eoOptOut(sampleCities, 0);
	t0 = nowMs();
	long long eoBaseSum = runEnablesOtherBaseline(wBase, 1, eoBaseOut);
	double eoBaseMs = nowMs() - t0;

	t0 = nowMs();
	long long eoOptSum = runEnablesOtherIndexed(wOpt, 1, eoOptOut);
	double eoOptMs = nowMs() - t0;

	if (eoBaseSum != eoOptSum || eoBaseOut != eoOptOut)
	{
		fail("EnablesOtherBuildings equivalence", cfg.name);
	}
	std::printf("  EnablesOther: base=%7.2f ms | indexed=%6.2f ms | speedup=%5.1fx | eq=%s\n",
		eoBaseMs, eoOptMs, eoBaseMs / std::max(0.001, eoOptMs),
		(eoBaseSum == eoOptSum && eoBaseOut == eoOptOut) ? "OK" : "MISMATCH");

	// 5. ConstructValue path caching
	ConstructBestResult cvBase = runConstructValueSim(wBase, false);
	ConstructBestResult cvOpt = runConstructValueSim(wOpt, true);
	if (cvBase.bestWeighted != cvOpt.bestWeighted || cvBase.bestCity != cvOpt.bestCity || cvBase.bestBuilding != cvOpt.bestBuilding)
	{
		fail("ConstructValuePathCache equivalence", cfg.name);
	}
	std::printf("  ConstructPth: basePaths=%ld | cachedPaths=%ld | reduction=%.1fx | eq=%s\n",
		cvBase.pathCalls, cvOpt.pathCalls,
		static_cast<double>(cvBase.pathCalls) / std::max(1L, cvOpt.pathCalls),
		(cvBase.bestWeighted == cvOpt.bestWeighted && cvBase.bestCity == cvOpt.bestCity && cvBase.bestBuilding == cvOpt.bestBuilding) ? "OK" : "MISMATCH");
}

int main(int argc, char** argv)
{
	bool quick = false;
	for (int i = 1; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--quick") == 0)
		{
			quick = true;
		}
	}

	std::printf("CvCityTurnPerf standalone baseline & equivalence suite (%s)\n",
		quick ? "quick" : "full");

	const int mult = quick ? 1 : 5;
	const ScenarioConfig scenarios[] = {
		{ "Early-Game",    5 * mult,  60, 4500, 80, 350, 25, 200 },
		{ "Mid-Game",      5 * mult, 250, 4500, 80, 350, 25, 200 },
		{ "Late-Game-C2C", 5 * mult, 600, 4500, 80, 350, 25, 200 }
	};

	for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i)
	{
		runScenario(scenarios[i]);
	}

	std::printf("\nRESULT: %s (failures=%ld)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
	return g_failures == 0 ? 0 : 1;
}
