//	Standalone tests for the CvReachablePlotSet dest-valid fallback.
//
//	Two things are tested, and they point in opposite directions:
//
//	A. Result equivalence (would-be sound): `ContextFreeNewPathValidFunc` marks the edge
//	   valid-as-terminus *before* evaluating the reveal/danger/move-through predicates, so for a
//	   BFS edge the decision `bValid || bValidAsTerminus` is already fixed once the domain and ZoC
//	   gates pass. If `NewPathDestValid` were a pure predicate, the fallback call could be skipped
//	   for those edges. Tests 1-3 verify that under the purity assumption, and test 2 shows the
//	   flag-before-internal ordering is what makes it hold.
//
//	B. The assumption is false in the engine (test 4): `NewPathDestValid` calls
//	   `CvPlayerAI::AI_getAnyPlotDanger`, which mutates the shared plot-danger LRU cache
//	   (`plotDangerCache`, `iLastUseCount`, `plotDangerCacheReads`), and `getActivePlayerSafeRangeCache`.
//	   Skipping a lookup changes which entries survive, so a later read by another caller can return
//	   a different value. This is not a modelling nicety - the pinned-save A/B showed it: per-turn
//	   REACH counts matched for the first 50 player-turn rows and then diverged. So the fallback
//	   must still be called, and the BFS only counts the dead calls (REACH_DEAD), it does not skip them.
//
//	g++ -O2 -std=c++11 -Wall -Wextra -o dest_tests CvReachableDestFallbackTests.cpp
//	./dest_tests          # full sweeps
//	./dest_tests --quick  # shrunk sweeps for interactive runs

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

static uint32_t g_rng = 0x9E3779B9u;

static uint32_t rnd()
{
	g_rng = g_rng * 1664525u + 1013904223u;
	return g_rng;
}

//	Transcription of Sources/CvMap.h
static int coordRange(int iCoord, int iRange, bool bWrap)
{
	if (bWrap && iRange != 0)
	{
		if (iCoord < 0)
		{
			return iRange + (iCoord % iRange);
		}
		if (iCoord >= iRange)
		{
			return iCoord % iRange;
		}
	}
	return iCoord;
}

static const int INVALID_PLOT_COORD = -2147483647 - 1;

//	Hex adjacency offsets (resolved through CvMap::plot, which wraps, so the BFS only ever
	//	sees real plot objects).
static const int DIR_X[10] = { 0, 1, 1, 0, -1, -1, 1, -1, 0, 0 };
static const int DIR_Y[10] = { -1, -1, 0, 1, 0, 1, 1, -1, 0, 0 };

struct MapModel
{
	int w, h;
	bool wrapX, wrapY;
	std::vector<int> plotX, plotY;

	MapModel(int iW, int iH, bool bWrapX, bool bWrapY) : w(iW), h(iH), wrapX(bWrapX), wrapY(bWrapY)
	{
		plotX.resize((size_t)iW * iH);
		plotY.resize((size_t)iW * h);
		for (int y = 0; y < h; ++y)
		{
			for (int x = 0; x < w; ++x)
			{
				plotX[(size_t)y * w + x] = x;
				plotY[(size_t)y * w + x] = y;
			}
		}
	}

	bool isPlot(int iX, int iY) const { return iX >= 0 && iX < w && iY >= 0 && iY < h; }
	int plotNum(int iX, int iY) const { return iY * w + iX; }

	int plot(int iX, int iY) const
	{
		if (iX == INVALID_PLOT_COORD || iY == INVALID_PLOT_COORD)
		{
			return -1;
		}
		int iMapX = coordRange(iX, w, wrapX);
		int iMapY = coordRange(iY, h, wrapY);
		return isPlot(iMapX, iMapY) ? plotNum(iMapX, iMapY) : -1;
	}

	int plotSorenINLINE(int iX, int iY) const
	{
		if (iX == INVALID_PLOT_COORD || iY == INVALID_PLOT_COORD)
		{
			return -1;
		}
		return plotNum(iX, iY);
	}
};

struct PlotState
{
	bool gatesPass;			// domain + ZoC gates
	bool internalResult;	// pathValidInternal (reveal / move-through / danger)
	bool destValidResult;	// NewPathDestValid
};

struct Effects
{
	int cacheWrites;		// pathValidInternal's observable plot validity-cache write
	int destValidCalls;
};

//	Pre-optimization decision, transcribed in source order.
static bool preEdge(const PlotState& p, bool& bFlag, Effects& eff, bool flagSetBeforeInternal)
{
	bFlag = false;
	bool bValid;

	if (!p.gatesPass)
	{
		bValid = false;				// gate returns before the flag is set
	}
	else
	{
		if (flagSetBeforeInternal)
		{
			bFlag = true;			// *pbValidAsTerminus = true (source order)
			bValid = p.internalResult;
			eff.cacheWrites += 1;
		}
		else
		{
			bValid = p.internalResult;
			eff.cacheWrites += 1;
			if (bValid)
			{
				bFlag = true;		// hypothetical alternative ordering
			}
		}
	}

	if (!bValid)
	{
		eff.destValidCalls += 1;
		bFlag |= p.destValidResult;
	}

	return bValid || bFlag;
}

//	Hypothetical short-circuit form (valid only if dest-valid is pure).
static bool postEdge(const PlotState& p, bool& bFlag, Effects& eff)
{
	bFlag = false;
	bool bValid;

	if (!p.gatesPass)
	{
		bValid = false;
	}
	else
	{
		bFlag = true;
		bValid = p.internalResult;
		eff.cacheWrites += 1;
	}

	if (!bValid && !bFlag)
	{
		eff.destValidCalls += 1;
		bFlag = p.destValidResult;
	}

	return bValid || bFlag;
}

static int g_failures = 0;

static void check(bool cond, const char* szWhat)
{
	if (!cond)
	{
		++g_failures;
		if (g_failures <= 5)
		{
			printf("  FAIL: %s\n", szWhat);
		}
	}
}

static void testTruthTable()
{
	printf("[1] decision equivalence under the purity assumption (full truth table)\n");
	for (int g = 0; g < 2; ++g)
	{
		for (int i = 0; i < 2; ++i)
		{
			for (int d = 0; d < 2; ++d)
			{
				PlotState p = { g != 0, i != 0, d != 0 };
				bool f1 = false, f2 = false;
				Effects e1 = { 0, 0 }, e2 = { 0, 0 };

				bool a = preEdge(p, f1, e1, true);
				bool b = postEdge(p, f2, e2);

				check(a == b && f1 == f2, "decision differs under the purity assumption");
				check(e1.cacheWrites == e2.cacheWrites, "retained cache-write trace differs");

				if (a)
				{
					check(f1, "valid edge without the terminus flag set");
				}
			}
		}
	}

	printf("[2] the flag-before-internal ordering is load-bearing\n");
	int diverged = 0;
	for (int g = 0; g < 2; ++g)
	{
		for (int i = 0; i < 2; ++i)
		{
			for (int d = 0; d < 2; ++d)
			{
				PlotState p = { g != 0, i != 0, d != 0 };
				bool f1 = false, f2 = false;
				Effects e1 = { 0, 0 }, e2 = { 0, 0 };
				if (preEdge(p, f1, e1, false) != postEdge(p, f2, e2))
				{
					++diverged;
				}
			}
		}
	}
	check(diverged > 0, "reverse ordering is indistinguishable, so the ordering is not proven load-bearing");
	printf("  reverse-ordering divergences: %d (expected > 0)\n", diverged);
}

static void testBfsEquivalence(int iMaps, int iRange)
{
	printf("[3] end-to-end BFS equivalence under the purity assumption\n");

	const int sizes[4] = { 5, 11, 23, 41 };

	for (int s = 0; s < 4; ++s)
	{
		for (int wrapX = 0; wrapX < 2; ++wrapX)
		{
			for (int wrapY = 0; wrapY < 2; ++wrapY)
			{
				MapModel m(sizes[s], sizes[s], wrapX != 0, wrapY != 0);

				for (int seed = 0; seed < iMaps / 4; ++seed)
				{
					g_rng = 0x1234567u + (uint32_t)(seed * 131 + sizes[s]);

					std::vector<PlotState> st((size_t)m.w * m.h);
					for (size_t p = 0; p < st.size(); ++p)
					{
						unsigned int r = rnd();
						st[p].gatesPass = (r & 1u) != 0;
						st[p].internalResult = (r & 2u) != 0;
						st[p].destValidResult = (r & 4u) != 0;
					}

					int iStart = (int)(rnd() % (uint32_t)(m.w * m.h));

					std::map<int, int> before, after;
					std::vector<int> orderBefore, orderAfter;
					Effects eBefore = { 0, 0 }, eAfter = { 0, 0 };

					std::vector<std::pair<int,int> > ring, next;
					before[iStart] = 0;
					after[iStart] = 0;
					orderBefore.push_back(iStart);
					orderAfter.push_back(iStart);
					ring.push_back(std::make_pair(iStart, 0));

					int depth = 0;
					while (depth < iRange && !ring.empty())
					{
						++depth;
						for (size_t r = 0; r < ring.size(); ++r)
						{
							for (int d = 0; d < 10; ++d)
							{
								int nb = m.plot(m.plotX[ring[r].first] + DIR_X[d], m.plotY[ring[r].first] + DIR_Y[d]);
								if (nb < 0)
								{
									continue;
								}

								if (before.find(nb) == before.end())
								{
									bool flag = false;
									if (preEdge(st[(size_t)nb], flag, eBefore, true))
									{
										before[nb] = depth;
										orderBefore.push_back(nb);
										next.push_back(std::make_pair(nb, depth));
									}
								}

								if (after.find(nb) == after.end())
								{
									bool flag = false;
									if (postEdge(st[(size_t)nb], flag, eAfter))
									{
										after[nb] = depth;
										orderAfter.push_back(nb);
									}
								}
							}
						}
						ring = next;
						next.clear();
					}

					check(before == after, "reachable set differs under the purity assumption");
					check(orderBefore == orderAfter, "insertion order differs under the purity assumption");
					check(eBefore.cacheWrites == eAfter.cacheWrites, "retained cache-write trace differs in BFS");
				}
			}
		}
	}
}

//	4: the purity assumption does NOT hold in the engine. Model the shared plot-danger LRU
	//	(PLOT_DANGER_CACHE_SIZE entries, keyed by plot, value captured at populate time, LRU by
	//	access count). Skipping a lookup changes which entries survive, so a later read of a plot
	//	whose state changed in between returns a different value.
static void testDangerLruIsObservable(int iCases)
{
	printf("[4] plot-danger LRU makes the skipped call observable (assumption fails)\n");

	const int LRU = 16;

	int diverged = 0;
	for (int c = 0; c < iCases; ++c)
	{
		g_rng = 0x9E3779B9u + (uint32_t)c;

		//	Shared cache: plot -> value captured when it was populated
		std::map<int, int> cachePre, cachePost;
		std::map<int, int> useCountPre, useCountPost;
		int callsPre = 0, callsPost = 0;

		//	Plots whose danger changes between population and a later read
		std::vector<int> order;
		for (int p = 0; p < 40; ++p)
		{
			order.push_back(p);
		}

		//	Dest-valid reads for plots whose terminus flag was already set. In the pre form every
			//	such plot populates the cache; in the post form those plots are skipped, so the
		//	entries that would have been there are instead populated later (with a changed value)
		//	or evicted.
		std::vector<bool> flagAlreadySet(40);
		for (int p = 0; p < 40; ++p)
		{
			flagAlreadySet[p] = (rnd() & 1u) != 0;
		}

		//	Phase 1: BFS reads, in the same order, both forms
		for (size_t i = 0; i < order.size(); ++i)
		{
			int p = order[i];
			int valAtRead = (int)(rnd() % 100);

			//	pre form: dest-valid runs for every edge that failed pathValidInternal
			bool preRuns = (rnd() & 2u) != 0;
			if (preRuns)
			{
				++callsPre;
				if ((int)cachePre.size() >= LRU && cachePre.find(p) == cachePre.end())
				{
					//	evict the least recently used
					int worst = -1, worstUse = 0x7FFFFFFF;
					for (std::map<int,int>::const_iterator it = cachePre.begin(); it != cachePre.end(); ++it)
					{
						if (useCountPre[it->first] < worstUse)
						{
							worstUse = useCountPre[it->first];
							worst = it->first;
						}
					}
					if (worst >= 0)
					{
						cachePre.erase(worst);
						useCountPre.erase(worst);
					}
				}
				cachePre[p] = valAtRead;
				++useCountPre[p];
			}

			//	post form: skipped when the terminus flag was already set
			if (!flagAlreadySet[p])
			{
				++callsPost;
				if ((int)cachePost.size() >= LRU && cachePost.find(p) == cachePost.end())
				{
					int worst = -1, worstUse = 0x7FFFFFFF;
					for (std::map<int,int>::const_iterator it = cachePost.begin(); it != cachePost.end(); ++it)
					{
						if (useCountPost[it->first] < worstUse)
						{
							worstUse = useCountPost[it->first];
							worst = it->first;
						}
					}
					if (worst >= 0)
					{
						cachePost.erase(worst);
						useCountPost.erase(worst);
					}
				}
				cachePost[p] = valAtRead;
				++useCountPost[p];
			}
		}

		//	Phase 2: a later caller reads a plot whose state changed after phase 1. The cached value
		//	(first writer wins) differs between the two forms whenever the entry still exists.
		for (size_t i = 0; i < order.size(); ++i)
		{
			int p = order[i];
			int newVal = (int)(rnd() % 100);

			bool preHit = cachePre.find(p) != cachePre.end();
			bool postHit = cachePost.find(p) != cachePost.end();

			int preVal = preHit ? cachePre[p] : newVal;
			int postVal = postHit ? cachePost[p] : newVal;

			if (preVal != postVal)
			{
				++diverged;
			}
		}

		check(callsPost <= callsPre, "post form calls dest-valid more often");
	}

	printf("  later-read divergences: %d over %d cases -> the skipped call IS observable\n", diverged, iCases);
	check(diverged > 0, "no divergence, so the purity assumption would be safe");
}

int main(int argc, char** argv)
{
	bool quick = argc > 1 && std::strcmp(argv[1], "--quick") == 0;
	int maps = quick ? 20 : 200;
	int cases = quick ? 20000 : 200000;

	testTruthTable();
	testBfsEquivalence(maps, quick ? 6 : 14);
	testDangerLruIsObservable(cases);

	if (g_failures == 0)
	{
		printf("RESULT: PASS\n");
		return 0;
	}

	printf("RESULT: FAIL (%d failures)\n", g_failures);
	return 1;
}
