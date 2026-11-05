// ponderosa: walk a streamed map of tiles built by `uv run ponderosa build`.
//
//   ponderosa [--tiles build/tiles] [--time 0.45] [--width W --height H] [--radius M]
//         [--east M --north M] [--yaw D --pitch D]
//         [--headless [--walk-ahead SECONDS] [--run] [--pass-east M] [--pass-north M]
//                     [--screenshot out.png] [--bench FRAMES]]
//
// --pass-east and --pass-north move in a straight line through everything, to prove that tiles
// load and unload on the way; the run fails if none unloaded.
//
// Controls: WASD walk, Shift run, mouse or arrows look, [ ] time of day, P screenshot,
// Esc frees the mouse (click to take it back), Esc again quits.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "settings.h"
#include "streaming.h"
#include "strata/strata.h"
#include "tile.h"
#include "walker.h"

#ifndef PONDEROSA_SOURCE_DIR
#define PONDEROSA_SOURCE_DIR "."
#endif

namespace {

using Clock = std::chrono::steady_clock;
double ms(Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); }

constexpr int kA = 4, kD = 7, kE = 8, kF = 9, kP = 19, kQ = 20, kSpace = 44, kLeftCtrl = 224, kPageUp = 75, kPageDown = 78, kS = 22, kW = 26, kEscape = 41, kLeftBracket = 47, kRightBracket = 48,
              kRight = 79, kLeft = 80, kDown = 81, kUp = 82;

struct Args {
    std::string tiles = std::string(PONDEROSA_SOURCE_DIR) + "/build/tiles", screenshot;
    bool fly = false;
    float altitude = 60;  // metres above the ground when --fly starts
    float time = 0.45f, yaw = 0, pitch = 0, radius = 700, walkAhead = 0, east = 0, north = 0, passEast = 0, passNorth = 0;
    int width = 1280, height = 720, bench = 0;
    bool headless = false, run = false;
};

bool parse(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : "0"; };
        if (k == "--tiles") a.tiles = val();
        else if (k == "--time") a.time = static_cast<float>(std::atof(val()));
        else if (k == "--yaw") a.yaw = static_cast<float>(std::atof(val()));
        else if (k == "--pitch") a.pitch = static_cast<float>(std::atof(val()));
        else if (k == "--fly") a.fly = true;
        else if (k == "--altitude") a.altitude = static_cast<float>(std::atof(val()));
        else if (k == "--radius") a.radius = static_cast<float>(std::atof(val()));
        else if (k == "--east") a.east = static_cast<float>(std::atof(val()));
        else if (k == "--north") a.north = static_cast<float>(std::atof(val()));
        else if (k == "--pass-east") a.passEast = static_cast<float>(std::atof(val()));
        else if (k == "--pass-north") a.passNorth = static_cast<float>(std::atof(val()));
        else if (k == "--walk-ahead") a.walkAhead = static_cast<float>(std::atof(val()));
        else if (k == "--width") a.width = std::atoi(val());
        else if (k == "--height") a.height = std::atoi(val());
        else if (k == "--bench") a.bench = std::atoi(val());
        else if (k == "--headless") a.headless = true;
        else if (k == "--run") a.run = true;
        else if (k == "--screenshot") a.screenshot = val();
        else {
            std::fprintf(stderr, "unknown argument %s\n", k.c_str());
            return false;
        }
    }
    return a.width > 0 && a.height > 0;
}

bool check(StrataStatus s, const char* what) {
    if (s == STRATA_OK) return true;
    std::fprintf(stderr, "ponderosa: %s: %s (%s)\n", what, strata_status_string(s), strata_last_error());
    return false;
}

float linear(uint8_t srgb) { return std::pow(static_cast<float>(srgb) / 255.0f, 2.2f); }

// The tiles in memory and on the GPU. World x, z are metres from the north-west corner of the start tile.
class Stream {
public:
    Stream(StrataEngine* engine, const ponderosa::Manifest& m, ponderosa::TileKey origin, ponderosa::StreamConfig c)
        : engine_(engine), m_(m), origin_(origin), config_(c), sorted_(m.tiles) {
        for (int i = 0; i < 256; ++i) linear_[i] = linear(static_cast<uint8_t>(i));
        std::sort(sorted_.begin(), sorted_.end());
    }

    float offsetX(ponderosa::TileKey k) const { return static_cast<float>(k.ix - origin_.ix) * m_.tileSize; }
    float offsetZ(ponderosa::TileKey k) const { return static_cast<float>(origin_.iy - k.iy) * m_.tileSize; }
    double east(float x) const { return origin_.ix * static_cast<double>(m_.tileSize) + x; }
    double north(float z) const { return (origin_.iy + 1) * static_cast<double>(m_.tileSize) - z; }
    ponderosa::TileKey keyAt(float x, float z) const {
        return {static_cast<int>(std::floor(east(x) / m_.tileSize)), static_cast<int>(std::floor(north(z) / m_.tileSize))};
    }

    // Applies the plan for a position, loading at most maxLoads tiles.
    bool update(float x, float z, int maxLoads) {
        std::set<ponderosa::TileKey> loaded;
        for (const auto& t : tiles_) loaded.insert(t.first);
        const ponderosa::StreamPlan plan =
            ponderosa::planStreaming(m_.tiles, loaded, east(x), north(z), m_.tileSize, config_);
        for (const ponderosa::TileKey& k : plan.unload) drop(k);
        int count = 0;
        for (const ponderosa::TileKey& k : plan.load) {
            if (count++ >= maxLoads) break;
            if (!load(k)) return false;
        }
        return true;
    }

    bool walkable(float x, float z) const { return tiles_.count(keyAt(x, z)) != 0; }
    // Flight goes wherever there is land to see, which is further than the tiles in memory.
    bool flyable(float x, float z) const {
        return walkable(x, z) || (overview_ ? overview_->covers(east(x), north(z)) : false);
    }
    float groundAt(float x, float z) const {
        const ponderosa::TileKey k = keyAt(x, z);
        const auto t = tiles_.find(k);
        if (t == tiles_.end()) return overview_ && !overview_->empty() ? overview_->heightAt(east(x), north(z)) : 0;
        return t->second.tile.heightAt(x - offsetX(k), z - offsetZ(k));
    }
    void setOverview(const ponderosa::Overview* o) { overview_ = o; }
    bool overTile(float x, float z) const { return std::binary_search(sorted_.begin(), sorted_.end(), keyAt(x, z)); }
    const ponderosa::Solids& solids() const { return solids_; }
    const std::vector<StrataRenderItem>& trees() const { return trees_; }
    int meshes() const { return meshCount_; }
    int peakMeshes() const { return peakMeshes_; }
    int tileCount() const { return static_cast<int>(tiles_.size()); }
    int loads() const { return loads_; }
    int unloads() const { return unloads_; }
    double readMs() const { return readMs_; }
    double uploadMs() const { return uploadMs_; }
    double worstLoadMs() const { return worstLoadMs_; }

private:
    struct Loaded {
        ponderosa::Tile tile;
        std::vector<uint32_t> ids;
    };

    bool add(const StrataMesh& mesh, Loaded& l) {
        uint32_t id = 0;
        if (!check(strata_add_mesh(engine_, &mesh, &id), "mesh")) return false;
        l.ids.push_back(id);
        ++meshCount_;
        peakMeshes_ = std::max(peakMeshes_, meshCount_);
        return true;
    }

    bool addTerrain(Loaded& l, float ox, float oz) {
        const ponderosa::Tile& t = l.tile;
        const int n = t.samples;
        const size_t count = static_cast<size_t>(n) * static_cast<size_t>(n);
        std::vector<float> pos(count * 3), nrm(count * 3), col(count * 3);
        for (int r = 0; r < n; ++r) {
            for (int c = 0; c < n; ++c) {
                const size_t i = (static_cast<size_t>(r) * static_cast<size_t>(n) + static_cast<size_t>(c)) * 3;
                pos[i] = ox + static_cast<float>(c) * t.cell;
                pos[i + 1] = t.height(c, r);
                pos[i + 2] = oz + static_cast<float>(r) * t.cell;
                const float dx = t.height(c - 1, r) - t.height(c + 1, r), dz = t.height(c, r - 1) - t.height(c, r + 1);
                const float up = 2 * t.cell, len = std::sqrt(dx * dx + up * up + dz * dz);
                nrm[i] = dx / len;
                nrm[i + 1] = up / len;
                nrm[i + 2] = dz / len;
                for (int k = 0; k < 3; ++k) col[i + static_cast<size_t>(k)] = linear_[t.colors[i + static_cast<size_t>(k)]];
            }
        }
        std::vector<uint32_t> idx;
        idx.reserve(static_cast<size_t>(n - 1) * static_cast<size_t>(n - 1) * 6);
        for (int r = 0; r + 1 < n; ++r) {
            for (int c = 0; c + 1 < n; ++c) {
                const uint32_t a = static_cast<uint32_t>(r * n + c), b = a + 1, d = a + static_cast<uint32_t>(n), e = d + 1;
                idx.insert(idx.end(), {a, d, b, b, d, e});
            }
        }
        StrataMesh mesh{};
        mesh.positions = pos.data();
        mesh.normals = nrm.data();
        mesh.colors = col.data();
        mesh.indices = idx.data();
        mesh.vertex_count = static_cast<int32_t>(count);
        mesh.index_count = static_cast<int32_t>(idx.size());
        return add(mesh, l);
    }

    bool load(ponderosa::TileKey k) {
        const auto t0 = Clock::now();
        Loaded l;
        std::string err;
        if (!ponderosa::loadTile(m_.path(k), l.tile, &err)) {
            std::fprintf(stderr, "ponderosa: %s\n", err.c_str());
            return false;
        }
        const double read = ms(t0);
        const auto t1 = Clock::now();
        const float ox = offsetX(k), oz = offsetZ(k);
        if (!addTerrain(l, ox, oz)) return false;
        for (const ponderosa::TileMesh& m : l.tile.meshes) {
            const size_t count = m.positions.size() / 3;
            std::vector<float> pos(m.positions), nrm(count * 3), col(count * 3);
            for (size_t i = 0; i < count; ++i) {
                pos[i * 3] += ox;
                pos[i * 3 + 2] += oz;
                for (size_t c = 0; c < 3; ++c) {
                    nrm[i * 3 + c] = static_cast<float>(m.normals[i * 4 + c]) / 127.0f;
                    col[i * 3 + c] = linear_[m.colors[i * 4 + c]];
                }
            }
            StrataMesh mesh{};
            mesh.positions = pos.data();
            mesh.normals = nrm.data();
            mesh.colors = col.data();
            mesh.indices = m.indices.data();
            mesh.vertex_count = static_cast<int32_t>(count);
            mesh.index_count = static_cast<int32_t>(m.indices.size());
            if (!add(mesh, l)) return false;
        }
        solids_.add(l.tile, ox, oz);
        tiles_[k] = std::move(l);
        rebuildTrees();
        const double upload = ms(t1);
        readMs_ += read;
        uploadMs_ += upload;
        worstLoadMs_ = std::max(worstLoadMs_, read + upload);
        ++loads_;
        return true;
    }

    void drop(ponderosa::TileKey k) {
        const auto t = tiles_.find(k);
        if (t == tiles_.end()) return;
        for (uint32_t id : t->second.ids) {
            check(strata_remove_mesh(engine_, id), "remove mesh");
            --meshCount_;
        }
        solids_.remove(k);
        tiles_.erase(t);
        ++unloads_;
        rebuildTrees();
    }

    void rebuildTrees() {
        trees_.clear();
        for (const auto& entry : tiles_) {
            const float ox = offsetX(entry.first), oz = offsetZ(entry.first);
            for (const ponderosa::Tree& t : entry.second.tile.trees) {
                const float trunk = t.height * 0.45f, crown = t.height * 0.7f;
                trees_.push_back({STRATA_PRIMITIVE_CYLINDER, {t.x + ox, t.y + trunk * 0.5f, t.z + oz},
                                  {ponderosa::kTrunkRadius, trunk * 0.5f, ponderosa::kTrunkRadius}, {0.105f, 0.065f, 0.034f, 1}});
                trees_.push_back({STRATA_PRIMITIVE_SPHERE, {t.x + ox, t.y + t.height - crown * 0.5f, t.z + oz},
                                  {t.crown, crown, t.crown}, {t.r, t.g, t.b, 1}});
            }
        }
    }

    StrataEngine* engine_;
    const ponderosa::Manifest& m_;
    ponderosa::TileKey origin_;
    ponderosa::StreamConfig config_;
    std::vector<ponderosa::TileKey> sorted_;
    const ponderosa::Overview* overview_ = nullptr;
    float linear_[256];
    std::map<ponderosa::TileKey, Loaded> tiles_;
    ponderosa::Solids solids_;
    std::vector<StrataRenderItem> trees_;
    int meshCount_ = 0, peakMeshes_ = 0, loads_ = 0, unloads_ = 0;
    double readMs_ = 0, uploadMs_ = 0, worstLoadMs_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parse(argc, argv, a)) return 2;
    ponderosa::Manifest manifest;
    std::string err;
    if (!ponderosa::loadManifest(a.tiles, manifest, &err)) {
        std::fprintf(stderr, "ponderosa: %s\n", err.c_str());
        return 1;
    }
    if (strata_abi_version() != STRATA_ABI_VERSION || strata_abi_minor() < 8) {
        std::fprintf(stderr, "ponderosa: Strata %s is too old (needs ABI 1.8 for mouse look)\n", strata_version_string());
        return 1;
    }
    StrataEngineDesc desc{};
    desc.struct_size = sizeof desc;
    desc.abi_version = STRATA_ABI_VERSION;
    desc.surface = a.headless ? STRATA_SURFACE_HEADLESS : STRATA_SURFACE_WINDOW;
    desc.width = a.width;
    desc.height = a.height;
    desc.flags = a.headless ? 0u : static_cast<uint32_t>(STRATA_FLAG_VSYNC);
    desc.title = "ponderosa";
    StrataEngine* engine = nullptr;
    if (!check(strata_create(&desc, &engine), "cannot start the renderer")) return 1;
    std::printf("Strata %s on %s\n", strata_version_string(), strata_gpu_description(engine));

    const double startEast = manifest.startEast + a.east, startNorth = manifest.startNorth + a.north;
    const ponderosa::TileKey origin{static_cast<int>(std::floor(startEast / manifest.tileSize)),
                                static_cast<int>(std::floor(startNorth / manifest.tileSize))};
    ponderosa::StreamConfig config;
    config.loadRadius = a.radius;
    config.unloadRadius = a.radius + 200;
    Stream stream(engine, manifest, origin, config);

    ponderosa::Walker walker;
    walker.x = static_cast<float>(startEast - origin.ix * static_cast<double>(manifest.tileSize));
    walker.z = static_cast<float>((origin.iy + 1) * static_cast<double>(manifest.tileSize) - startNorth);
    walker.yawDeg = a.yaw;
    walker.pitchDeg = a.pitch;
    ponderosa::Overview overview;
    bool ok = true;
    if (!manifest.overview.empty()) {
        ok = ponderosa::loadOverview(manifest.dir + "/" + manifest.overview, overview, &err);
        if (!ok) std::fprintf(stderr, "ponderosa: %s\n", err.c_str());
    }
    if (ok && !overview.empty()) {
        // Strata lays samples along +x then +z, which is how the file runs: from the north-west, rows to the south.
        std::vector<float> colors(overview.colors.size());
        for (size_t i = 0; i < colors.size(); ++i) colors[i] = linear(overview.colors[i]);
        StrataTerrainTile tile{};
        tile.origin[0] = static_cast<float>(overview.east - origin.ix * static_cast<double>(manifest.tileSize));
        tile.origin[2] = static_cast<float>((origin.iy + 1) * static_cast<double>(manifest.tileSize) - overview.north);
        tile.cell_size = overview.cell;
        tile.samples_x = overview.cols;
        tile.samples_z = overview.rows;
        tile.heights = overview.heights.data();
        tile.colors = colors.data();
        ok = check(strata_set_terrain(engine, &tile), "overview");
        stream.setOverview(&overview);
    }
    ok = ok && stream.update(walker.x, walker.z, 1000);
    if (ok) {
        // A start point inside a building moves to open ground.
        for (float r = 0; r < 60 && stream.solids().inside(walker.x, walker.z); r += 1.0f) walker.x += 1.0f;
        walker.eyeY = stream.groundAt(walker.x, walker.z) + ponderosa::kEyeHeight;
    }

    StrataLighting light{};
    strata_lighting_defaults(STRATA_LIGHTING_SUN, &light);
    light.latitude_degrees = static_cast<float>(manifest.latitude);
    light.time_of_day = a.time;
    light.cascade_count = 4;
    light.shadow_map_size = 2048;
    light.shadow_distance = 300.0f;
    light.split_lambda = 0.8f;
    light.fog = 1;

    int w = a.width, h = a.height;
    auto draw = [&]() -> bool {
        float ahead[3];
        walker.forward(ahead);
        const float eye[3] = {walker.x, walker.eyeY, walker.z};
        const float look[3] = {eye[0] + ahead[0], eye[1] + ahead[1], eye[2] + ahead[2]};
        const float up[3] = {0, 1, 0};
        StrataCamera c{};
        float nearPlane, farPlane;
        const float aboveGround = walker.eyeY - stream.groundAt(walker.x, walker.z);
        ponderosa::clipPlanes(aboveGround, nearPlane, farPlane);
        light.shadow_distance = std::max(300.0f, aboveGround * 3.0f);
        strata_camera_look_at(eye, look, up, 70.0f, static_cast<float>(w) / static_cast<float>(std::max(1, h)), nearPlane, farPlane, &c);
        StrataFrameDesc fd{};
        fd.clear_flags = STRATA_CLEAR_COLOR | STRATA_CLEAR_DEPTH | STRATA_CLEAR_SKY;
        const auto& trees = stream.trees();
        return check(strata_set_camera(engine, &c), "camera") && check(strata_set_lighting(engine, &light), "lighting") &&
               check(strata_begin_frame(engine, &fd), "begin_frame") &&
               check(strata_submit_primitives(engine, trees.data(), static_cast<int32_t>(trees.size())), "trees") &&
               check(strata_end_frame(engine), "end_frame");
    };
    bool flying = a.fly;
    float flySpeed = ponderosa::kFlySpeed, walkSpeed = ponderosa::kWalkSpeed;
    auto walkableHere = [&](float x, float z) { return stream.walkable(x, z); };
    auto flyableHere = [&](float x, float z) { return stream.flyable(x, z); };
    auto groundHere = [&](float x, float z) { return stream.groundAt(x, z); };
    if (flying) walker.eyeY = stream.groundAt(walker.x, walker.z) + a.altitude;
    auto toggleFlight = [&] {
        if (flying && !stream.walkable(walker.x, walker.z)) {
            std::puts("no streets here to land on");  // past the city limits, or tiles still loading
            return;
        }
        flying = !flying;
        if (flying) walker.eyeY += ponderosa::kFlyClearance;  // lift off at once
        else walker.clearOfBuildings(stream.solids(), walkableHere);
        std::puts(flying ? "flying" : "on foot");
    };
    auto stepWalker = [&](float ahead, float right, bool run, float dt, float up = 0) {
        if (flying) {
            walker.fly(flyableHere, groundHere, ahead, right, up, run, dt, flySpeed);
            return;
        }
        walker.step(stream.solids(), [&](float x, float z) { return stream.walkable(x, z); }, ahead, right, run, dt, walkSpeed);
        const float target = stream.groundAt(walker.x, walker.z) + ponderosa::kEyeHeight;
        walker.eyeY += (target - walker.eyeY) * std::min(1.0f, dt * 10.0f);
    };

    if (a.headless) {
        const float dt = 1.0f / 60.0f;
        for (int i = 0, steps = static_cast<int>(a.walkAhead * 60.0f); ok && i < steps; ++i) {
            stepWalker(1, 0, a.run, dt);
            ok = stream.update(walker.x, walker.z, 1);
        }
        const float passLength = std::hypot(a.passEast, a.passNorth);
        if (ok && passLength > 0) {
            const int steps = static_cast<int>(passLength * 2);  // half a metre at a time, a tile at most per step
            const float x0 = walker.x, z0 = walker.z;
            for (int i = 1; ok && i <= steps; ++i) {
                const float u = static_cast<float>(i) / static_cast<float>(steps);
                walker.x = x0 + a.passEast * u;
                walker.z = z0 - a.passNorth * u;
                ok = stream.update(walker.x, walker.z, 1);
            }
            walker.eyeY = stream.groundAt(walker.x, walker.z) + ponderosa::kEyeHeight;
            if (stream.unloads() == 0 || stream.peakMeshes() > STRATA_MAX_MESHES) {
                std::fprintf(stderr, "ponderosa: the pass did not stream (%d unloads, peak %d meshes)\n", stream.unloads(), stream.peakMeshes());
                ok = false;
            }
        }
        if (ok && a.bench > 0) {
            const auto t0 = Clock::now();
            for (int i = 0; ok && i < a.bench; ++i) {
                walker.look(360.0f / static_cast<float>(a.bench), 0);
                ok = draw();
            }
            std::printf("bench: %.1f ms a frame over %d frames, a full turn\n", ms(t0) / a.bench, a.bench);
        }
        if (ok && !a.screenshot.empty()) ok = draw() && check(strata_save_screenshot_png(engine, a.screenshot.c_str()), "screenshot");
        StrataFrameStats st{};
        strata_get_frame_stats(engine, &st);
        std::printf("at %.1f, %.1f (tile %d %d); %d tiles held, %d meshes, peak %d of %d; %d loads, %d unloads, read %.1f ms, upload "
                    "%.1f ms each, worst %.1f ms; %d draw calls\n",
                    static_cast<double>(walker.x), static_cast<double>(walker.z), stream.keyAt(walker.x, walker.z).ix,
                    stream.keyAt(walker.x, walker.z).iy, stream.tileCount(), stream.meshes(), stream.peakMeshes(),
                    STRATA_MAX_MESHES, stream.loads(), stream.unloads(), stream.readMs() / std::max(1, stream.loads()),
                    stream.uploadMs() / std::max(1, stream.loads()), stream.worstLoadMs(), st.draw_calls);
        strata_destroy(engine);
        return ok ? 0 : 1;
    }

    std::puts("WASD walk, Shift run, F fly (E up, Q down), mouse or arrows look, [ ] time, P screenshot, Esc frees the mouse, Esc again quits");
    bool running = ok, mouseHeld = false;
    auto changeSpeed = [&](int notches) {
        if (flying) flySpeed = ponderosa::stepSpeed(flySpeed, notches, ponderosa::kMinFlySpeed, ponderosa::kMaxFlySpeed);
        else walkSpeed = ponderosa::stepSpeed(walkSpeed, notches, ponderosa::kMinWalkSpeed, ponderosa::kMaxWalkSpeed);
    };
    auto holdMouse = [&](bool hold) { mouseHeld = check(strata_set_relative_mouse_mode(engine, hold ? 1 : 0), "mouse look") && hold; };
    holdMouse(true);
    bool held[256] = {};
    int shots = 0;
    auto last = Clock::now();
    float sinceLoad = 0;
    while (running) {
        StrataEvent e;
        while (strata_poll_event(engine, &e)) {
            switch (e.type) {
            case STRATA_EVENT_QUIT: running = false; break;
            case STRATA_EVENT_RESIZED: strata_get_drawable_size(engine, &w, &h); break;
            case STRATA_EVENT_KEY_DOWN:
                if (e.scancode >= 0 && e.scancode < 256) held[e.scancode] = true;
                if (e.scancode == kPageUp) changeSpeed(1);
                else if (e.scancode == kPageDown) changeSpeed(-1);
                if (e.repeat) break;
                if (e.scancode == kEscape) {
                    if (mouseHeld) holdMouse(false);
                    else running = false;
                } else if (e.scancode == kF) {
                    toggleFlight();
                } else if (e.scancode == kP) {
                    char name[64];
                    std::snprintf(name, sizeof name, "ponderosa-%03d.png", shots++);
                    if (check(strata_save_screenshot_png(engine, name), "screenshot")) std::printf("saved %s\n", name);
                }
                break;
            case STRATA_EVENT_KEY_UP:
                if (e.scancode >= 0 && e.scancode < 256) held[e.scancode] = false;
                break;
            case STRATA_EVENT_MOUSE_BUTTON_DOWN:
                if (!mouseHeld) holdMouse(true);
                break;
            case STRATA_EVENT_MOUSE_WHEEL:
                if (e.y != 0) changeSpeed(e.y > 0 ? 1 : -1);
                break;
            case STRATA_EVENT_MOUSE_MOTION:
                if (mouseHeld) walker.look(e.dx * 0.15f, -e.dy * 0.15f);
                break;
            default: break;
            }
        }
        const auto now = Clock::now();
        const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
        last = now;
        walker.look(((held[kRight] ? 1.0f : 0.0f) - (held[kLeft] ? 1.0f : 0.0f)) * 90.0f * dt,
                    ((held[kUp] ? 1.0f : 0.0f) - (held[kDown] ? 1.0f : 0.0f)) * 60.0f * dt);
        stepWalker((held[kW] ? 1.0f : 0.0f) - (held[kS] ? 1.0f : 0.0f), (held[kD] ? 1.0f : 0.0f) - (held[kA] ? 1.0f : 0.0f),
                   held[225] || held[229], dt,
                   (held[kE] || held[kSpace] ? 1.0f : 0.0f) - (held[kQ] || held[kLeftCtrl] ? 1.0f : 0.0f));
        if (held[kLeftBracket]) light.time_of_day -= dt * 0.05f;
        if (held[kRightBracket]) light.time_of_day += dt * 0.05f;
        light.time_of_day -= std::floor(light.time_of_day);
        sinceLoad += dt;
        const int loadsBefore = stream.loads();
        const bool missing = stream.overTile(walker.x, walker.z) && !stream.walkable(walker.x, walker.z);
        if (!stream.update(walker.x, walker.z, ponderosa::loadDue(sinceLoad, missing) ? 1 : 0) || !draw() ||
            !check(strata_present(engine), "present"))
            running = false;
        if (stream.loads() != loadsBefore) sinceLoad = 0;
    }
    strata_destroy(engine);
    return 0;
}
