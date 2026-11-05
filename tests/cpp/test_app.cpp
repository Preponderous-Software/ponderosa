// Unit tests for everything that needs no GPU: the tile and manifest readers, streaming and walking.
#include <algorithm>
#include <cmath>
#include <set>
#include <vector>
#include <cstdio>
#include <string>

#include "settings.h"
#include "streaming.h"
#include "tile.h"
#include "walker.h"

static int failures = 0;
#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

using ponderosa::TileKey;

static std::vector<TileKey> grid(int across) {
    std::vector<TileKey> out;
    for (int y = 0; y < across; ++y) {
        for (int x = 0; x < across; ++x) out.push_back({x, y});
    }
    return out;
}

static bool has(const std::vector<TileKey>& v, TileKey k) { return std::find(v.begin(), v.end(), k) != v.end(); }

static void testStreaming() {
    CHECK(ponderosa::tileDistance({0, 0}, 250, 250, 500) == 0);
    CHECK(std::fabs(ponderosa::tileDistance({1, 0}, 250, 250, 500) - 250) < 1e-9);
    CHECK(std::fabs(ponderosa::tileDistance({1, 1}, 400, 400, 500) - std::hypot(100.0, 100.0)) < 1e-9);

    ponderosa::StreamConfig c;
    c.loadRadius = 300;
    c.unloadRadius = 400;
    c.maxTiles = 60;
    const auto all = grid(10);
    // From nothing: the tile underfoot comes first, then only what is in range.
    auto p = ponderosa::planStreaming(all, {}, 2250, 2250, 500, c);
    CHECK(!p.load.empty() && p.load[0] == (TileKey{4, 4}));
    CHECK(p.load.size() == 5 && p.unload.empty());  // the tile and its four sides; corners are 353 m off
    CHECK(!has(p.load, {2, 4}));

    // Loaded tiles are not asked for again.
    std::set<TileKey> loaded(p.load.begin(), p.load.end());
    CHECK(ponderosa::planStreaming(all, loaded, 2250, 2250, 500, c).load.empty());

    // Hysteresis: a tile past the load radius but inside the unload radius stays, so an edge walk does not thrash.
    std::set<TileKey> one{{0, 0}};
    CHECK(ponderosa::planStreaming(all, one, 850, 250, 500, c).unload.empty());
    CHECK(has(ponderosa::planStreaming(all, one, 901, 250, 500, c).unload, {0, 0}));

    // The budget caps what loads, keeps the nearest, and counts tiles being unloaded as free.
    c.maxTiles = 2;
    p = ponderosa::planStreaming(all, {}, 2250, 2250, 500, c);
    CHECK(p.load.size() == 2 && p.load[0] == (TileKey{4, 4}));
    std::set<TileKey> full{{9, 9}, {4, 4}};
    p = ponderosa::planStreaming(all, full, 2250, 2250, 500, c);
    CHECK(p.unload.size() == 1 && p.load.size() == 1);

    // Off the data: nothing to load, and no crash.
    c.maxTiles = 60;
    CHECK(ponderosa::planStreaming(all, {}, -5000, -5000, 500, c).load.empty());
}

static ponderosa::Tile squareTile() {
    ponderosa::Tile t;
    t.key = {3, 7};
    t.size = 20;
    t.cell = 1;
    t.samples = 21;
    for (int r = -1; r <= 21; ++r) {
        for (int c = -1; c <= 21; ++c) t.heights.push_back(static_cast<float>(c) * 0.1f);
    }
    t.footprints.push_back({{8, 8, 12, 8, 12, 12, 8, 12}});
    t.trees.push_back({4, 0, 4, 9, 3, 0, 1, 0});
    return t;
}

static void testWalking() {
    const ponderosa::Tile t = squareTile();
    CHECK(std::fabs(t.heightAt(5, 5) - 0.5f) < 1e-5f);
    CHECK(std::fabs(t.heightAt(5.5f, 2) - 0.55f) < 1e-5f);
    ponderosa::Solids solids;
    solids.add(t, 100, 200);
    CHECK(solids.inside(110, 210) && !solids.inside(107, 210));
    auto anywhere = [](float, float) { return true; };

    {
        // Flight: over walls, toward the view, never into the ground, and only as far as the map goes.
        auto ground = [](float x, float) { return x * 0.01f; };
        auto mapped = [](float x, float) { return x < 1000.0f; };
        ponderosa::Walker f;
        f.x = 100;
        f.z = 210;
        f.eyeY = 50;
        f.yawDeg = 90;
        f.fly(mapped, ground, 1, 0, 0, false, 1.0f);
        CHECK(std::fabs(f.x - (100 + ponderosa::kFlySpeed)) < 1e-3f && std::fabs(f.eyeY - 50) < 1e-3f);
        CHECK(solids.inside(110, 210));  // the flight above went straight through this building
        f.fly(mapped, ground, 1, 0, 0, true, 1.0f);
        CHECK(std::fabs(f.x - (120 + ponderosa::kFlyFastSpeed)) < 1e-3f);
        f.fly(mapped, ground, 0, 1, 0, false, 1.0f);  // right of east is south
        CHECK(std::fabs(f.z - (210 + ponderosa::kFlySpeed)) < 1e-3f);
        f.pitchDeg = -45;  // diving goes down as well as along
        const float before = f.eyeY;
        f.fly(mapped, ground, 1, 0, 0, false, 0.5f);
        CHECK(f.eyeY < before);
        f.pitchDeg = 0;
        f.fly(mapped, ground, 0, 0, -1, true, 100.0f);
        CHECK(std::fabs(f.eyeY - (ground(f.x, f.z) + ponderosa::kFlyClearance)) < 1e-3f);
        f.fly(mapped, ground, 0, 0, 1, true, 100.0f);
        CHECK(std::fabs(f.eyeY - (ground(f.x, f.z) + ponderosa::kFlyCeiling)) < 1e-3f);
        f.fly(mapped, ground, 1, 0, 0, true, 100.0f);
        CHECK(f.x < 1000);  // the edge of the map stops a flight
        const float z0 = f.z;
        f.yawDeg = 135;  // and it can be slid along
        f.fly(mapped, ground, 1, 0, 0, false, 1.0f);
        CHECK(f.z > z0 && f.x < 1000);

        {
            // A jump rises about a metre, comes down, and cannot be repeated in the air.
            ponderosa::Walker j;
            j.jump();
            float top = 0;
            int frames = 0;
            for (; frames < 300 && (frames == 0 || j.hop > 0); ++frames) {
                if (frames == 10) j.jump();
                j.stepHop(1.0f / 60.0f);
                top = std::max(top, j.hop);
            }
            CHECK(top > 1.0f && top < 1.3f);
            CHECK(j.hop == 0 && j.hopSpeed == 0 && frames > 40 && frames < 70);
            j.jump();
            CHECK(j.hopSpeed == ponderosa::kJumpSpeed);
        }
        ponderosa::Walker quick;  // A chosen speed, and Shift scaling it.
        quick.x = 100;
        quick.yawDeg = 90;
        quick.fly(mapped, ground, 1, 0, 0, false, 1.0f, 200.0f);
        CHECK(std::fabs(quick.x - 300) < 1e-2f);
        quick.fly(mapped, ground, 1, 0, 0, true, 1.0f, 100.0f);
        CHECK(std::fabs(quick.x - 700) < 1e-2f);
        CHECK(ponderosa::stepViewRadius(700, 1) == 800.0f && ponderosa::stepViewRadius(700, -1) == 600.0f);
        CHECK(ponderosa::stepViewRadius(1500, 1) == ponderosa::kMaxViewRadius && ponderosa::stepViewRadius(200, -1) == ponderosa::kMinViewRadius);
        CHECK(ponderosa::stepViewRadius(733, 1) == 800.0f);  // a --radius between steps lands on one
        {
            // The longest view fits Strata's mesh limit anywhere on a full map, with nothing dropped by the budget.
            std::vector<ponderosa::TileKey> all;
            for (int y = 0; y < 40; ++y)
                for (int x = 0; x < 40; ++x) all.push_back({x, y});
            ponderosa::StreamConfig longest;
            longest.loadRadius = ponderosa::kMaxViewRadius;
            longest.unloadRadius = ponderosa::kMaxViewRadius + 200;
            const auto plan = ponderosa::planStreaming(all, {}, 10000, 10000, 500, longest);
            size_t reach = 0;
            for (const auto& k : all) reach += ponderosa::tileDistance(k, 10000, 10000, 500) <= ponderosa::kMaxViewRadius;
            CHECK(reach <= static_cast<size_t>(longest.maxTiles) && plan.load.size() == reach);
        }
        CHECK(ponderosa::stepFov(70, 1) == 75.0f && ponderosa::stepFov(110, 1) == ponderosa::kMaxFov && ponderosa::stepFov(40, -1) == ponderosa::kMinFov);
        {
            // Settings come back as they went in, and a bad file cannot do harm.
            ponderosa::Settings chosen;
            chosen.flySpeed = 152.59f;
            chosen.viewRadius = 1200;
            chosen.sensitivity = 0.075f;
            chosen.fov = 95;
            chosen.panel = false;
            const ponderosa::Settings back = ponderosa::parseSettings(ponderosa::formatSettings(chosen));
            CHECK(std::fabs(back.flySpeed - chosen.flySpeed) < 0.01f && back.viewRadius == 1200 && back.fov == 95 && !back.panel);
            CHECK(std::fabs(back.sensitivity - chosen.sensitivity) < 0.001f);
            const ponderosa::Settings bad = ponderosa::parseSettings("fly_speed 99999\nfov banana\nmystery 3\nview_radius -5\n");
            CHECK(bad.flySpeed == ponderosa::kMaxFlySpeed && bad.fov == 70 && bad.viewRadius == ponderosa::kMinViewRadius);
            CHECK(ponderosa::parseSettings("").flySpeed == 20);
        }
        CHECK(ponderosa::frameStep(0.016f) == 0.016f && ponderosa::frameStep(0.2f) == 0.2f && ponderosa::frameStep(3.0f) == 0.25f);
        CHECK(std::string(ponderosa::compass(0)) == "N" && std::string(ponderosa::compass(359)) == "N");
        CHECK(std::string(ponderosa::compass(90)) == "E" && std::string(ponderosa::compass(225)) == "SW");
        const std::string panel = ponderosa::hudText({true, 40, 700, 120, 90, 58, 11});
        CHECK(panel.find("flying  40 m/s") != std::string::npos && panel.find("view    700 m") != std::string::npos);
        CHECK(panel.find("facing  E") != std::string::npos && panel.find("tiles   11") != std::string::npos);
        CHECK(ponderosa::hudText({false, 1.4f, 700, 1.65f, 0, 60, 4}).find("on foot  1.4 m/s") != std::string::npos);
        CHECK(ponderosa::stepSpeed(20, 1, 5, 800) == 25.0f);
        CHECK(std::fabs(ponderosa::stepSpeed(25, -1, 5, 800) - 20.0f) < 1e-4f);
        CHECK(ponderosa::stepSpeed(790, 1, 5, 800) == 800.0f && ponderosa::stepSpeed(5, -1, 5, 800) == 5.0f);

        ponderosa::Walker land;  // Landing on a roof steps off it.
        land.x = 110;
        land.z = 210;
        CHECK(solids.inside(land.x, land.z));
        land.clearOfBuildings(solids, anywhere);
        CHECK(!solids.inside(land.x, land.z));
    }

    {
        // A city is not a rectangle: a ring of 1000-odd tiles with a hole and an arm, flown across at full speed.
        std::vector<ponderosa::TileKey> city;
        for (int y = 0; y < 44; ++y) {
            for (int x = 0; x < 44; ++x) {
                const bool hole = x > 18 && x < 26 && y > 18 && y < 26, arm = x > 30 && y < 30;
                if (!hole && !arm) city.push_back({x, y});
            }
        }
        std::sort(city.begin(), city.end());
        CHECK(city.size() > 1000);
        ponderosa::StreamConfig config;
        std::set<ponderosa::TileKey> held;
        size_t peak = 0;
        const double size = 500;
        double far = 0;
        for (double t = 0; t < 280; t += 0.12) {  // one load every 0.12 s, moving 80 m/s, west to east
            const double e = t * 80, n = 11000;
            const ponderosa::StreamPlan plan = ponderosa::planStreaming(city, held, e, n, size, config);
            for (const auto& k : plan.unload) held.erase(k);
            if (!plan.load.empty()) {
                CHECK(std::binary_search(city.begin(), city.end(), plan.load[0]));  // never a tile that is not there
                held.insert(plan.load[0]);
            }
            for (const auto& k : held) far = std::max(far, ponderosa::tileDistance(k, e, n, size));
            peak = std::max(peak, held.size());
            CHECK(4 * held.size() <= 240);  // four meshes a tile, under Strata's 256
        }
        CHECK(peak > 8 && far <= config.unloadRadius + 80);
        CHECK(ponderosa::loadDue(0.2f, false) && !ponderosa::loadDue(0.01f, false) && ponderosa::loadDue(0.01f, true));
        float nearPlane, farPlane;
        ponderosa::clipPlanes(1.65f, nearPlane, farPlane);
        CHECK(nearPlane == 0.1f);
        ponderosa::clipPlanes(400, nearPlane, farPlane);
        CHECK(nearPlane == 20.0f && farPlane > 30000);

        // The overview: heights interpolate, and a point only counts as covered inside it.
        ponderosa::Overview o;
        o.cols = 3;
        o.rows = 2;
        o.cell = 20;
        o.east = 1000;
        o.north = 2000;
        o.heights = {0, 10, 20, 30, 40, 50};
        CHECK(std::fabs(o.heightAt(1010, 2000) - 5) < 1e-3f && std::fabs(o.heightAt(1000, 1990) - 15) < 1e-3f);
        CHECK(std::fabs(o.heightAt(9999, 0) - 50) < 1e-3f);
        CHECK(o.covers(1040, 1980) && !o.covers(1041, 1980) && !o.covers(1000, 2001) && !ponderosa::Overview{}.covers(0, 0));
        ponderosa::Overview read;
        std::string overviewError;
        CHECK(!ponderosa::loadOverview("no-such-overview.bin", read, &overviewError) && !overviewError.empty());
    }

    ponderosa::Walker p;
    p.x = 102;
    p.z = 210;
    p.yawDeg = 90;
    float f[3];
    p.forward(f);
    CHECK(f[0] > 0.99f);
    for (int i = 0; i < 60 * 10; ++i) p.step(solids, anywhere, 1, 0, false, 1.0f / 60.0f);
    CHECK(std::fabs(p.x - (108 - ponderosa::kBodyRadius)) < 1e-3f);  // stopped at the wall
    p.step(solids, anywhere, 1, 1, true, 0.3f);                   // a long frame slides along it
    CHECK(p.z > 210.5f && p.x < 108 && !solids.inside(p.x, p.z));

    ponderosa::Walker q;
    q.x = 101;
    q.z = 204;
    q.yawDeg = 90;
    for (int i = 0; i < 60 * 5; ++i) q.step(solids, anywhere, 1, 0, false, 1.0f / 60.0f);
    CHECK(std::fabs(q.x - (104 - ponderosa::kTrunkRadius - ponderosa::kBodyRadius)) < 1e-3f);  // a trunk

    ponderosa::Walker e;  // Pace, and the edge of what is loaded.
    e.x = 0;
    e.yawDeg = 90;
    for (int i = 0; i < 60; ++i) e.step(solids, anywhere, 1, 0, false, 1.0f / 60.0f);
    CHECK(std::fabs(e.x - ponderosa::kWalkSpeed) < 1e-3f);
    for (int i = 0; i < 60; ++i) e.step(solids, [](float x, float) { return x < 2.0f; }, 1, 0, true, 1.0f / 60.0f);
    CHECK(e.x < 2.0f && e.x > 1.8f);

    // A tile's walls leave with it.
    solids.remove(t.key);
    CHECK(!solids.inside(110, 210));
}

static void testFiles() {
    ponderosa::Manifest m;
    std::string err;
    CHECK(!ponderosa::loadManifest("no-such-dir", m, &err) && !err.empty());
    ponderosa::Tile t;
    CHECK(!ponderosa::loadTile("no-such.tile", t, &err));

    const std::string dir = std::string(PONDEROSA_SOURCE_DIR) + "/build/tiles";
    if (!ponderosa::loadManifest(dir, m, &err)) {
        std::puts("build/tiles not built; skipping the real-tile checks");
        return;
    }
    CHECK(m.tileSize == 500 && m.tiles.size() == 16 && std::fabs(m.latitude - 40.6) < 1);
    const TileKey k = m.tiles[5];
    CHECK(ponderosa::loadTile(m.path(k), t, &err));
    CHECK(t.key == k && t.samples == 251 && t.meshes.size() >= 1);
    CHECK(t.heights.size() == 253u * 253u && t.colors.size() == 251u * 251u * 3);

    // A truncated copy is refused.
    std::FILE* in = std::fopen(m.path(k).c_str(), "rb");
    std::fseek(in, 0, SEEK_END);
    std::string bytes(static_cast<size_t>(std::ftell(in)), '\0');
    std::fseek(in, 0, SEEK_SET);
    CHECK(std::fread(&bytes[0], 1, bytes.size(), in) == bytes.size());
    std::fclose(in);
    std::FILE* out = std::fopen("truncated.tile", "wb");
    std::fwrite(bytes.data(), 1, bytes.size() / 2, out);
    std::fclose(out);
    ponderosa::Tile bad;
    CHECK(!ponderosa::loadTile("truncated.tile", bad, &err));
    std::remove("truncated.tile");

    // Neighbours agree along their shared edge, where a seam would show.
    if (m.has({k.ix + 1, k.iy})) {
        ponderosa::Tile east;
        CHECK(ponderosa::loadTile(m.path({k.ix + 1, k.iy}), east, &err));
        float worst = 0;
        for (int r = 0; r < t.samples; ++r) worst = std::max(worst, std::fabs(t.height(t.samples - 1, r) - east.height(0, r)));
        CHECK(worst == 0);
    }
}

int main() {
    testStreaming();
    testWalking();
    testFiles();
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::puts("test_app: all checks passed");
    return failures ? 1 : 0;
}
