// CvRouteConnectivityTests.cpp
// Standalone equivalence + benchmark for the AI_updateRouteToCity optimization.
//
// Original semantics (CvCityAI::AI_updateRouteToCity in Caveman2Cosmos):
//   for every other same-area city B of the player's team:
//     RouteFinder FAStar GeneratePath(A -> B) where a node (parent!=NULL) is
//     valid iff (plot unowned OR plot team == player team) AND
//                plot.routeType == player.getBestRoute(plot)
//     start node is always valid (routeValid(parent=NULL) -> true).
//   pBestCity = nearest (strictly-smaller plotDistance, first wins ties,
//               team players iI ascending, city list order) UNREACHABLE city.
//
// Optimized semantics: the predicate is per-node, so reachability is an
// 8-connected component relation over predicate-true plots. Flood components
// once; A reaches B (A!=B) iff B is true and comp(B) is "adjacent" to A, i.e.
//   (A true && comp(A)==comp(B)) || some true 8-neighbor N of A has comp(N)==comp(B).
//
// Build:  g++ -O2 -std=c++11 -Wall -Wextra -o /tmp/opencode/route_conn_tests CvRouteConnectivityTests.cpp
// Run:    ./route_conn_tests --quick && ./route_conn_tests

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <random>
#include <ctime>
#include <vector>

// ---------------------------------------------------------------------------
// Mini world: grid with wrap flags, team per plot (-1 = unowned), route type,
// precomputed per-player best route (models m_eBestRoute cache results).
// ---------------------------------------------------------------------------
struct World
{
    int w, h;
    bool wrapX, wrapY;
    std::vector<int> team;      // -1 unowned
    std::vector<int> route;     // -1 none
    std::vector<int> bestRoute; // predicate: route[i] == bestRoute[i]
    std::vector<int> area;      // area id per plot

    int idx(int x, int y) const { return y * w + x; }

    // Wrapped/unwrapped neighbor coordinates exactly like Civ4 maps.
    bool wrapXY(int& x, int& y) const
    {
        if (x < 0)      { if (!wrapX) return false; x += w; }
        else if (x >= w){ if (!wrapX) return false; x -= w; }
        if (y < 0)      { if (!wrapY) return false; y += h; }
        else if (y >= h){ if (!wrapY) return false; y -= h; }
        return true;
    }

    bool pred(int i) const { return route[i] == bestRoute[i]; }
    // predicate including ownership half
    bool predFull(int i, int playerTeam) const
    {
        return (team[i] == -1 || team[i] == playerTeam) && route[i] == bestRoute[i];
    }
};

// Model of FAStar GeneratePath(A->B) with routeValid: BFS full exploration.
// Start node always valid; every *entered* node must satisfy predFull.
static bool oldReachable(const World& W, int a, int b, int playerTeam)
{
    if (a == b) return true;
    std::vector<char> vis(W.w * W.h, 0);
    std::queue<int> q;
    q.push(a);
    vis[a] = 1;
    const int dx[8] = {-1, 0, 1, 0, -1, 1, -1, 1};
    const int dy[8] = { 0,-1, 0,  1, -1, 1, -1, 1};
    while (!q.empty())
    {
        int u = q.front(); q.pop();
        int ux = u % W.w, uy = u / W.w;
        for (int k = 0; k < 8; ++k)
        {
            int nx = ux + dx[k], ny = uy + dy[k];
            if (!W.wrapXY(nx, ny)) continue;
            int v = W.idx(nx, ny);
            if (vis[v]) continue;
            if (!W.predFull(v, playerTeam)) continue;
            if (v == b) return true;
            vis[v] = 1;
            q.push(v);
        }
    }
    return false;
}

static int oldPlotDistance(int ax, int ay, int bx, int by)
{
    // Civ4 plotDistance: max of dx,dy (Chebyshev)
    int dx = ax > bx ? ax - bx : bx - ax;
    int dy = ay > by ? ay - by : by - ay;
    return dx > dy ? dx : dy;
}

struct City { int x, y, areaId; };

// Original selection: iterate team players iI ascending, each player's city list;
// here cities carry an explicit order index (stable combined order).
// Returns chosen city index or -1.
static int oldBestCity(const World& W, const std::vector<City>& cities,
                       int selfIdx, int playerTeam)
{
    int bestDist = 0x7fffffff;
    int best = -1;
    for (int i = 0; i < (int)cities.size(); ++i)
    {
        const City& c = cities[i];
        if (i == selfIdx) continue;
        if (c.areaId != cities[selfIdx].areaId) continue;
        int a = W.idx(cities[selfIdx].x, cities[selfIdx].y);
        int b = W.idx(c.x, c.y);
        if (!oldReachable(W, a, b, playerTeam))
        {
            int d = oldPlotDistance(cities[selfIdx].x, cities[selfIdx].y, c.x, c.y);
            if (d < bestDist) { bestDist = d; best = i; }
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// New flood-comp model.
// ---------------------------------------------------------------------------
struct FloodCache
{
    std::vector<int> comp; // 0 = not-true/unvisited-fail, >0 comp id
    void build(const World& W, int playerTeam)
    {
        const int n = W.w * W.h;
        comp.assign(n, 0);
        int nextComp = 0;
        const int dx[8] = {-1, 0, 1, 0, -1, 1, -1, 1};
        const int dy[8] = { 0,-1, 0,  1, -1, 1, -1, 1};
        std::vector<int> stack;
        for (int s = 0; s < n; ++s)
        {
            if (comp[s] != 0 || !W.predFull(s, playerTeam)) continue;
            ++nextComp;
            comp[s] = nextComp;
            stack.push_back(s);
            while (!stack.empty())
            {
                int u = stack.back(); stack.pop_back();
                int ux = u % W.w, uy = u / W.w;
                for (int k = 0; k < 8; ++k)
                {
                    int nx = ux + dx[k], ny = uy + dy[k];
                    if (!W.wrapXY(nx, ny)) continue;
                    int v = W.idx(nx, ny);
                    if (comp[v] != 0 || !W.predFull(v, playerTeam)) continue;
                    comp[v] = nextComp;
                    stack.push_back(v);
                }
            }
        }
    }

    bool reachable(const World& W, const FloodCache& /*self*/, int a, int b, int playerTeam) const
    {
        if (a == b) return true;
        if (!W.predFull(b, playerTeam)) return false;
        const int dx[8] = {-1, 0, 1, 0, -1, 1, -1, 1};
        const int dy[8] = { 0,-1, 0,  1, -1, 1, -1, 1};
        if (W.predFull(a, playerTeam) && comp[a] != 0) return comp[a] == comp[b];
        int ux = a % W.w, uy = a / W.w;
        for (int k = 0; k < 8; ++k)
        {
            int nx = ux + dx[k], ny = uy + dy[k];
            if (!W.wrapXY(nx, ny)) continue;
            int v = W.idx(nx, ny);
            if (comp[v] != 0 && comp[v] == comp[b]) return true;
        }
        return false;
    }
};

static int newBestCity(const World& W, const std::vector<City>& cities,
                       int selfIdx, int playerTeam, const FloodCache& fc)
{
    int bestDist = 0x7fffffff;
    int best = -1;
    const int a = W.idx(cities[selfIdx].x, cities[selfIdx].y);
    for (int i = 0; i < (int)cities.size(); ++i)
    {
        const City& c = cities[i];
        if (i == selfIdx) continue;
        if (c.areaId != cities[selfIdx].areaId) continue;
        int b = W.idx(c.x, c.y);
        if (!fc.reachable(W, fc, a, b, playerTeam))
        {
            int d = oldPlotDistance(cities[selfIdx].x, cities[selfIdx].y, c.x, c.y);
            if (d < bestDist) { bestDist = d; best = i; }
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Random scenario generator + harness
// ---------------------------------------------------------------------------
static void makeRandomWorld(World& W, std::mt19937& rng)
{
    std::uniform_int_distribution<int> dw(20, 60), dh(20, 60);
    W.w = dw(rng); W.h = dh(rng);
    W.wrapX = rng() % 4 == 0;
    W.wrapY = false;
    const int n = W.w * W.h;
    W.team.assign(n, -1);
    W.route.assign(n, -1);
    W.bestRoute.assign(n, 0);
    W.area.assign(n, 0);

    std::uniform_int_distribution<int> pLand(0, 9);
    std::uniform_int_distribution<int> pOwn(0, 9);
    std::uniform_int_distribution<int> pRoute(0, 9);
    std::uniform_int_distribution<int> teamDist(0, 2);

    // Two team blobs with simple areas: left half area 0, right half area 1.
    for (int y = 0; y < W.h; ++y)
        for (int x = 0; x < W.w; ++x)
        {
            int i = W.idx(x, y);
            if (pLand(rng) == 0) W.team[i] = teamDist(rng); // enemy-owned plot
            if (pOwn(rng) == 0) W.team[i] = 5;              // player-owned
            if (pRoute(rng) < 6) W.route[i] = 0;            // built route
            W.area[i] = (x < W.w / 2) ? 0 : 1;
        }
    // Player team = 5. bestRoute: mostly 0 (predicate true when route 0 built),
    // but sometimes 2 (route-less plots still satisfy if best==route==-1? no:
    // route -1 vs best 2 -> false) — introduce best=-1 patches:
    std::uniform_int_distribution<int> pPatch(0, 15);
    for (int i = 0; i < n; ++i)
        if (pPatch(rng) == 0) W.bestRoute[i] = -1;
}

static void makeCities(World& W, std::vector<City>& cities, std::mt19937& rng, int playerTeam)
{
    cities.clear();
    std::uniform_int_distribution<int> px(1, W.w - 2), py(1, W.h - 2);
    int tries = 0;
    while ((int)cities.size() < 12 && tries < 400)
    {
        ++tries;
        City c{px(rng), py(rng), 0};
        c.areaId = W.area[W.idx(c.x, c.y)];
        // city center: route it (like a city tile often has a route) so searches do work
        int i = W.idx(c.x, c.y);
        W.team[i] = playerTeam;
        W.route[i] = 0;
        W.bestRoute[i] = 0;
        cities.push_back(c);
    }
}

static long long randCase(int seed)
{
    std::mt19937 rng(seed);
    const int playerTeam = 5;
    World W;
    makeRandomWorld(W, rng);
    std::vector<City> cities;
    makeCities(W, cities, rng, playerTeam);
    FloodCache fc;
    fc.build(W, playerTeam);

    long long mismatches = 0;
    for (int s = 0; s < (int)cities.size(); ++s)
    {
        int oldSel = oldBestCity(W, cities, s, playerTeam);
        int newSel = newBestCity(W, cities, s, playerTeam, fc);
        if (oldSel != newSel)
        {
            ++mismatches;
            printf("  MISMATCH seed=%d self=%d old=%d new=%d\n", seed, s, oldSel, newSel);
        }
    }
    return mismatches;
}

// ---------------------------------------------------------------------------
// Benchmark: big map, many cities, old vs new
// ---------------------------------------------------------------------------
static void bench()
{
    const int W_ = 300, H_ = 200; // 60k plots
    World W;
    W.w = W_; W.h = H_; W.wrapX = W.wrapY = false;
    const int n = W_ * H_;
    W.team.assign(n, -1);
    W.route.assign(n, -1);
    W.bestRoute.assign(n, 0);
    W.area.assign(n, 0);
    std::mt19937 rng(99);
    // Pathology matching in-game: large route networks, cities separated by
    // cuts => most searches explore huge areas (failed searches in the old
    // model are full-component explorations, n^2 of them).
    for (int y = 0; y < H_; ++y)
        for (int x = 0; x < W_; ++x)
        {
            int i = W.idx(x, y);
            W.route[i] = 0; // fully routed map...
        }
    // ...cut by two unrouted gaps (predicate false => barrier), one diagonal:
    for (int y = 0; y < H_; ++y)
    {
        int gapX = 100 + (y / 8) % 30;
        W.route[W.idx(gapX, y)] = -1;      // route != best -> barrier column
        W.route[W.idx(W_ - 60, y)] = -1;   // second cut
    }
    std::vector<City> cities;
    std::uniform_int_distribution<int> px(5, W_ - 6), py(5, H_ - 6);
    for (int k = 0; k < 15; ++k)
    {
        City c{px(rng), py(rng), 0};
        int i = W.idx(c.x, c.y);
        W.route[i] = 0; W.bestRoute[i] = 0;
        cities.push_back(c);
    }

    clock_t t0 = clock();
    int oldHits = 0;
    for (int s = 0; s < (int)cities.size(); ++s)
        if (oldBestCity(W, cities, s, 5) >= 0) ++oldHits;
    clock_t t1 = clock();
    FloodCache fc; fc.build(W, 5);
    int newHits = 0;
    for (int s = 0; s < (int)cities.size(); ++s)
        if (newBestCity(W, cities, s, 5, fc) >= 0) ++newHits;
    clock_t t2 = clock();

    double oldMs = 1000.0 * (t1 - t0) / CLOCKS_PER_SEC;
    double newMs = 1000.0 * (t2 - t1) / CLOCKS_PER_SEC;
    printf("BENCH: old=%.1fms new=%.1fms speedup=%.1fx (hits %d/%d)\n",
           oldMs, newMs, oldMs / (newMs + 1e-9), oldHits, newHits);
    if (oldHits != newHits) printf("  BENCH mismatch!\n");
}

int main(int argc, char** argv)
{
    bool quick = argc > 1 && strcmp(argv[1], "--quick") == 0;
    int cases = quick ? 200 : 3000;
    long long totalMismatch = 0;
    long long totalChecks = 0;

    for (int s = 0; s < cases; ++s)
    {
        totalMismatch += randCase(s + 1);
        totalChecks += 12;
    }

    printf("random cases: %d, per-city checks: %lld, mismatches: %lld\n",
           cases, totalChecks, totalMismatch);
    bench();

    printf("RESULT: %s\n", totalMismatch == 0 ? "PASS" : "FAIL");
    return totalMismatch == 0 ? 0 : 1;
}
