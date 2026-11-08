// ponderosa: walk or fly a streamed map of tiles built by `uv run ponderosa build`.
// `ponderosa --help` lists the options, and H in the window lists the controls.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "ponderosa.h"
#include "settings.h"
#include "tile_stream.h"
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

constexpr int kA = 4, kD = 7, kE = 8, kF = 9, kP = 19, kQ = 20, kSpace = 44, kLeftCtrl = 224, kMinus = 45, kEquals = 46, k9 = 38, k0 = 39, kComma = 54, kPeriod = 55, kTab = 43, kPageUp = 75, kPageDown = 78, kH = 11, kI = 12, kR = 21, kZ = 29, kBackspace = 42, kS = 22, kW = 26, kEscape = 41, kLeftBracket = 47, kRightBracket = 48,
              kRight = 79, kLeft = 80, kDown = 81, kUp = 82;

constexpr float kGlyph = 16, kLineGap = 22;  // HUD text, in pixels

constexpr const char* kUsage =
    "ponderosa: walk or fly a map of tiles built by `uv run ponderosa build`\n"
    "\n"
    "  --tiles DIR        the tiles to open (default build/tiles)\n"
    "  --radius M         how far detailed tiles load (default 700, or your saved view)\n"
    "  --fly              start in the air, --altitude M above the ground (default 60)\n"
    "  --east M --north M start this far from the map's start point\n"
    "  --yaw D --pitch D  start facing this way; yaw 0 is north, 90 is east\n"
    "  --time T           time of day, 0 to 1 (default 0.45)\n"
    "  --width W --height H  window size\n"
    "  --help             this list\n"
    "\n"
    "Rendering without a window:\n"
    "  --headless         render and exit; saved settings are ignored\n"
    "  --screenshot FILE  write the frame to a PNG; --hud draws the status panel on it\n"
    "  --walk-ahead S     walk forward first, --run to run\n"
    "  --pass-east M --pass-north M  move in a straight line; fails if no tile unloaded\n"
    "  --bench FRAMES     time a full turn\n"
    "\n"
    "Press H in the window for the controls.\n";

struct Args {
    std::string tiles = std::string(PONDEROSA_SOURCE_DIR) + "/build/tiles", screenshot;
    bool fly = false;
    float altitude = 60;  // metres above the ground when --fly starts
    float time = 0.45f, yaw = 40, pitch = 0, radius = 700, walkAhead = 0, east = 0, north = 0, passEast = 0, passNorth = 0;
    int width = 1280, height = 720, bench = 0;
    bool headless = false, run = false, hud = false, radiusGiven = false;
};

bool parse(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        if (k == "--help" || k == "-h") {
            std::fputs(kUsage, stdout);
            std::exit(0);
        }
        const bool flag = k == "--fly" || k == "--headless" || k == "--run" || k == "--hud";
        if (!flag && k.rfind("--", 0) == 0 && i + 1 >= argc) {
            std::fprintf(stderr, "ponderosa: %s needs a value (--help lists the options)\n", k.c_str());
            return false;
        }
        auto val = [&]() -> const char* { return argv[++i]; };
        if (k == "--tiles") a.tiles = val();
        else if (k == "--time") a.time = static_cast<float>(std::atof(val()));
        else if (k == "--yaw") a.yaw = static_cast<float>(std::atof(val()));
        else if (k == "--pitch") a.pitch = static_cast<float>(std::atof(val()));
        else if (k == "--fly") a.fly = true;
        else if (k == "--altitude") a.altitude = static_cast<float>(std::atof(val()));
        else if (k == "--radius") {
            a.radius = static_cast<float>(std::atof(val()));
            a.radiusGiven = true;
        }
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
        else if (k == "--hud") a.hud = true;
        else if (k == "--screenshot") a.screenshot = val();
        else {
            std::fprintf(stderr, "ponderosa: unknown option %s (--help lists the options)\n", k.c_str());
            return false;
        }
    }
    return a.width > 0 && a.height > 0;
}

// %APPDATA%/ponderosa, or the XDG config folder. Outside the program's own folder, so a rebuild cannot lose it.
std::filesystem::path settingsPath() {
    if (const char* d = std::getenv("APPDATA")) return std::filesystem::path(d) / "ponderosa" / "settings.txt";
    if (const char* d = std::getenv("XDG_CONFIG_HOME")) return std::filesystem::path(d) / "ponderosa" / "settings.txt";
    if (const char* d = std::getenv("HOME")) return std::filesystem::path(d) / ".config" / "ponderosa" / "settings.txt";
    return {};
}

ponderosa::Settings loadSettings() {
    std::ifstream in(settingsPath());
    std::ostringstream text;
    text << in.rdbuf();
    return ponderosa::parseSettings(text.str());
}

void saveSettings(const ponderosa::Settings& s) {
    const std::filesystem::path path = settingsPath();
    if (path.empty()) return;
    std::error_code ignored;  // settings are a convenience: failing to keep them must not stop anything
    std::filesystem::create_directories(path.parent_path(), ignored);
    std::ofstream(path) << ponderosa::formatSettings(s);
}

bool check(StrataStatus s, const char* what) {
    if (s == STRATA_OK) return true;
    std::fprintf(stderr, "ponderosa: %s: %s (%s)\n", what, strata_status_string(s), strata_last_error());
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parse(argc, argv, a)) return 2;
    // A render has to come out the same for everyone, so only a window reads or writes saved settings.
    const ponderosa::Settings saved = a.headless ? ponderosa::Settings{} : loadSettings();
    if (!a.radiusGiven && !a.headless) a.radius = saved.viewRadius;
    ponderosa::Manifest manifest;
    std::string err;
    if (!ponderosa::loadManifest(a.tiles, manifest, &err)) {
        std::fprintf(stderr, "ponderosa: %s\n", err.c_str());
        return 1;
    }
    if (strata_abi_version() != STRATA_ABI_VERSION || strata_abi_minor() < 11) {
        std::fprintf(stderr, "ponderosa: Strata %s is too old (needs ABI 1.11 for the status panel)\n", strata_version_string());
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
    streetkit::Stream stream(engine, manifest, origin, config, [&](streetkit::TileKey k, streetkit::Tile& t, std::string* why) {
        return streetkit::loadTile(manifest.path(k), t, why) ? streetkit::TileFetch::Ready : streetkit::TileFetch::Failed;
    });

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
    if (ok && !overview.empty()) ok = stream.showOverview(overview);
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
    bool flying = a.fly, hud = a.hud || (!a.headless && saved.panel);
    float flySpeed = saved.flySpeed, walkSpeed = saved.walkSpeed, sensitivity = saved.sensitivity, fov = saved.fov, zoom = 1, fps = 60;
    bool invertLook = saved.invertLook;
    bool helpOpen = !a.headless, helpAtStart = helpOpen, mouseHeld = false;
    std::string note;  // a passing message: what just happened, or why it did not
    float noteLeft = 0;
    auto say = [&](const std::string& text) {
        std::puts(text.c_str());
        note = text;
        noteLeft = 3;
    };
    // One StrataText a line, because a '\n' inside one leaves no gap between lines.
    auto writeLines = [&](float x, float y, const std::string& text, const float color[4]) -> bool {
        std::vector<std::string> lines(1);
        for (char c : text) {
            if (c == '\n') lines.emplace_back();
            else lines.back() += c;
        }
        std::vector<StrataText> labels;
        for (size_t i = 0; i < lines.size(); ++i)
            labels.push_back({x, y + static_cast<float>(i) * kLineGap, kGlyph, STRATA_TEXT_SHADOW, {color[0], color[1], color[2], color[3]}, lines[i].c_str()});
        return check(strata_submit_text(engine, labels.data(), static_cast<int32_t>(labels.size())), "text");
    };
    auto overlay = [&]() -> bool {
        const float yellow[4] = {1, 0.9f, 0.5f, 1};
        if (noteLeft > 0 && !writeLines((static_cast<float>(w) - static_cast<float>(note.size()) * kGlyph) / 2, static_cast<float>(h) - 48, note, yellow))
            return false;
        if (!a.headless && !mouseHeld) {
            const std::string freed = "Click to look around. Esc again quits.";
            if (!writeLines((static_cast<float>(w) - static_cast<float>(freed.size()) * kGlyph) / 2, 7 * kLineGap + 34, freed, yellow)) return false;
        }
        if (helpOpen) {
            const float white[4] = {1, 1, 1, 1};
            const StrataRect panel{static_cast<float>(w) - 33 * kGlyph - 24, 8, 33 * kGlyph + 16, 19 * kLineGap + 10, {0, 0, 0, 0.5f}};
            if (!check(strata_submit_rects(engine, &panel, 1), "help panel") ||
                !writeLines(panel.x + 8, 14, ponderosa::kHelpText, white))
                return false;
        }
        if (!hud) return true;
        const ponderosa::HudState state{flying, flying ? flySpeed : walkSpeed, stream.viewRadius(),
                                        walker.eyeY + walker.hop - stream.groundAt(walker.x, walker.z), walker.yawDeg, fps, stream.tileCount(), stream.waiting(), light.time_of_day};
        const std::string text = ponderosa::hudText(state);
        const float white[4] = {1, 1, 1, 1};
        const StrataRect panel{8, 8, 20 * kGlyph + 16, 7 * kLineGap + 10, {0, 0, 0, 0.5f}};
        return check(strata_submit_rects(engine, &panel, 1), "status panel") && writeLines(16, 14, text, white);
    };
    auto draw = [&]() -> bool {
        float ahead[3];
        walker.forward(ahead);
        const float eye[3] = {walker.x, walker.eyeY + walker.hop, walker.z};
        const float look[3] = {eye[0] + ahead[0], eye[1] + ahead[1], eye[2] + ahead[2]};
        const float up[3] = {0, 1, 0};
        StrataCamera c{};
        float nearPlane, farPlane;
        const float aboveGround = walker.eyeY - stream.groundAt(walker.x, walker.z);
        ponderosa::clipPlanes(aboveGround, nearPlane, farPlane);
        light.shadow_distance = std::max(300.0f, aboveGround * 3.0f);
        strata_camera_look_at(eye, look, up, fov * zoom, static_cast<float>(w) / static_cast<float>(std::max(1, h)), nearPlane, farPlane, &c);
        StrataFrameDesc fd{};
        fd.clear_flags = STRATA_CLEAR_COLOR | STRATA_CLEAR_DEPTH | STRATA_CLEAR_SKY;
        const auto& trees = stream.trees();
        return check(strata_set_camera(engine, &c), "camera") && check(strata_set_lighting(engine, &light), "lighting") &&
               check(strata_begin_frame(engine, &fd), "begin_frame") &&
               check(strata_submit_primitives(engine, trees.data(), static_cast<int32_t>(trees.size())), "trees") && overlay() &&
               check(strata_end_frame(engine), "end_frame");
    };
    auto walkableHere = [&](float x, float z) { return stream.walkable(x, z); };
    auto flyableHere = [&](float x, float z) { return stream.flyable(x, z); };
    auto groundHere = [&](float x, float z) { return stream.groundAt(x, z); };
    if (flying) walker.eyeY = stream.groundAt(walker.x, walker.z) + a.altitude;
    auto toggleFlight = [&] {
        if (flying && !stream.walkable(walker.x, walker.z)) {
            say("No streets here to land on");  // past the city limits, or tiles still loading
            return;
        }
        flying = !flying;
        walker.hop = walker.hopSpeed = 0;
        if (flying) walker.eyeY += ponderosa::kFlyClearance;  // lift off at once
        else walker.clearOfBuildings(stream.solids(), walkableHere);
    };
    auto stepWalker = [&](float ahead, float right, bool run, float dt, float up = 0) {
        if (flying) {
            walker.fly(flyableHere, groundHere, ahead, right, up, run, dt, flySpeed);
            return;
        }
        walker.step(stream.solids(), [&](float x, float z) { return stream.walkable(x, z); }, ahead, right, run, dt, walkSpeed);
        const float target = stream.groundAt(walker.x, walker.z) + ponderosa::kEyeHeight;
        walker.eyeY += (target - walker.eyeY) * std::min(1.0f, dt * 10.0f);
        walker.stepHop(dt);
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

    std::puts("H shows the controls");
    bool running = ok;
    const float startX = walker.x, startZ = walker.z;
    auto changeSpeed = [&](int notches) {
        const float before = flying ? flySpeed : walkSpeed;
        if (flying) flySpeed = ponderosa::stepSpeed(flySpeed, notches, ponderosa::kMinFlySpeed, ponderosa::kMaxFlySpeed);
        else walkSpeed = ponderosa::stepSpeed(walkSpeed, notches, ponderosa::kMinWalkSpeed, ponderosa::kMaxWalkSpeed);
        if ((flying ? flySpeed : walkSpeed) == before) say(notches > 0 ? "That is the top speed" : "That is the lowest speed");
    };
    auto changeView = [&](int steps) {
        const float before = stream.viewRadius(), radius = ponderosa::stepViewRadius(before, steps);
        stream.setLoadRadius(radius);
        if (radius == before) say(steps > 0 ? "That is the longest view" : "That is the shortest view");
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
                else if (e.scancode == kComma || e.scancode == kPeriod) {
                    sensitivity = ponderosa::stepSpeed(sensitivity, e.scancode == kPeriod ? 1 : -1, ponderosa::kMinSensitivity, ponderosa::kMaxSensitivity);
                    char text[40];
                    std::snprintf(text, sizeof text, "Mouse sensitivity %.2f", static_cast<double>(sensitivity));
                    say(text);
                } else if (e.scancode == k9 || e.scancode == k0) {
                    fov = ponderosa::stepFov(fov, e.scancode == k0 ? 1 : -1);
                    say("Field of view " + std::to_string(static_cast<int>(fov)));
                } else if (e.scancode == kMinus) changeView(-1);
                else if (e.scancode == kEquals) changeView(1);
                if (e.repeat) break;
                if (helpAtStart && (e.scancode == kW || e.scancode == kA || e.scancode == kS || e.scancode == kD || e.scancode == kF)) {
                    helpOpen = helpAtStart = false;  // it has done its job, and H brings it back
                }
                if (e.scancode == kEscape) {
                    if (mouseHeld) holdMouse(false);
                    else running = false;
                } else if (e.scancode == kI) {
                    invertLook = !invertLook;
                    say(invertLook ? "Mouse look inverted" : "Mouse look normal");
                } else if (e.scancode == kBackspace) {
                    const ponderosa::Settings d;
                    flySpeed = d.flySpeed;
                    walkSpeed = d.walkSpeed;
                    sensitivity = d.sensitivity;
                    fov = d.fov;
                    invertLook = d.invertLook;
                    hud = d.panel;
                    stream.setLoadRadius(d.viewRadius);
                    say("Settings back to the defaults");
                } else if (e.scancode == kR) {
                    walker.x = startX;
                    walker.z = startZ;
                    walker.yawDeg = a.yaw;
                    walker.pitchDeg = a.pitch;
                    flying = false;
                    walker.hop = walker.hopSpeed = 0;
                    walker.eyeY = stream.groundAt(startX, startZ) + ponderosa::kEyeHeight;
                    say("Back at the start");
                } else if (e.scancode == kH) {
                    helpOpen = !helpOpen;
                    helpAtStart = false;
                } else if (e.scancode == kTab) {
                    hud = !hud;
                } else if (e.scancode == kF) {
                    toggleFlight();
                } else if (e.scancode == kP) {
                    char name[64];
                    std::snprintf(name, sizeof name, "ponderosa-%03d.png", shots++);
                    if (check(strata_save_screenshot_png(engine, name), "screenshot")) say(std::string("Saved ") + name);
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
                if (mouseHeld) walker.look(e.dx * sensitivity * zoom, (invertLook ? e.dy : -e.dy) * sensitivity * zoom);
                break;
            default: break;
            }
        }
        const auto now = Clock::now();
        const float elapsed = std::chrono::duration<float>(now - last).count(), dt = ponderosa::frameStep(elapsed);
        last = now;
        if (elapsed > 0) fps += (1.0f / elapsed - fps) * std::min(1.0f, elapsed * 2);  // about half a second of smoothing
        noteLeft -= dt;
        zoom += ((held[kZ] ? 0.3f : 1.0f) - zoom) * std::min(1.0f, dt * 12);
        walker.look(((held[kRight] ? 1.0f : 0.0f) - (held[kLeft] ? 1.0f : 0.0f)) * 90.0f * dt,
                    ((held[kUp] ? 1.0f : 0.0f) - (held[kDown] ? 1.0f : 0.0f)) * 60.0f * dt);
        if (!flying && held[kSpace]) walker.jump();
        const float ahead = (held[kW] ? 1.0f : 0.0f) - (held[kS] ? 1.0f : 0.0f), right = (held[kD] ? 1.0f : 0.0f) - (held[kA] ? 1.0f : 0.0f);
        const float rise = (held[kE] || held[kSpace] ? 1.0f : 0.0f) - (held[kQ] || held[kLeftCtrl] ? 1.0f : 0.0f);
        stepWalker(ahead, right, held[225] || held[229], dt, rise);
        if (noteLeft <= 0 && flying && rise > 0 && walker.eyeY - stream.groundAt(walker.x, walker.z) >= ponderosa::kFlyCeiling - 0.5f) {
            say("Highest you can fly: " + std::to_string(static_cast<int>(ponderosa::kFlyCeiling)) + " m above the ground");
        }
        // Say why a move stopped. Looks further than a frame's travel, or fast flight stops short of the edge and never sees it.
        if (noteLeft <= 0 && (ahead != 0 || right != 0)) {
            const float yaw = walker.yawDeg * 3.14159265f / 180.0f, fx = std::sin(yaw), fz = -std::cos(yaw);
            const float reach = std::max(10.0f, (flying ? flySpeed : walkSpeed) * 4 * dt * 2);
            const float nextX = walker.x + (ahead * fx - right * fz) * reach, nextZ = walker.z + (ahead * fz + right * fx) * reach;
            if (flying && !stream.flyable(nextX, nextZ)) say("The map ends here");
            else if (!flying && !stream.overTile(nextX, nextZ)) say("The streets end here. F flies on.");
        }
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
    saveSettings({flySpeed, walkSpeed, stream.viewRadius(), sensitivity, fov, hud, invertLook});
    strata_destroy(engine);
    return 0;
}
