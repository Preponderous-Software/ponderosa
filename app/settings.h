// What the player can change while playing. Pure: tested without a GPU.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace ponderosa {

constexpr float kMinFlySpeed = 5.0f, kMaxFlySpeed = 800.0f;    // metres a second
constexpr float kMinWalkSpeed = 0.7f, kMaxWalkSpeed = 14.0f;
constexpr float kMinSensitivity = 0.03f, kMaxSensitivity = 0.6f;   // degrees a pixel of mouse movement
constexpr float kMinFov = 40.0f, kMaxFov = 110.0f, kFovStep = 5.0f;  // vertical, in degrees
constexpr float kSpeedNotch = 1.25f;                           // one wheel notch
constexpr float kMinViewRadius = 200.0f, kMaxViewRadius = 1500.0f, kViewRadiusStep = 100.0f;  // 1500 m fits the tile budget

// A notch multiplies, so it feels the same at a stroll as at full speed.
inline float stepSpeed(float speed, int notches, float lowest, float highest) {
    return std::clamp(speed * std::pow(kSpeedNotch, static_cast<float>(notches)), lowest, highest);
}

inline float stepViewRadius(float radius, int steps) {
    const float snapped = std::round(radius / kViewRadiusStep) + static_cast<float>(steps);
    return std::clamp(snapped * kViewRadiusStep, kMinViewRadius, kMaxViewRadius);
}

constexpr const char* kHelpText =
    "WASD       move\n"
    "mouse      look (arrows too)\n"
    "Shift      go four times faster\n"
    "wheel      slower / faster\n"
    "- =        shorter / longer view\n"
    ", .        mouse sensitivity\n"
    "9 0        field of view\n"
    "F          fly / land\n"
    "E Q        up / down\n"
    "[ ]        time of day\n"
    "R          back to the start\n"
    "P          screenshot\n"
    "Tab        hide the panel\n"
    "H          hide this help\n"
    "Esc        free the mouse";

inline float stepFov(float fov, int steps) {
    return std::clamp(fov + static_cast<float>(steps) * kFovStep, kMinFov, kMaxFov);
}

// Yaw 0 faces north, 90 east.
inline const char* compass(float yawDeg) {
    static const char* const names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const float turns = yawDeg / 45.0f + 0.5f;
    return names[static_cast<int>(std::floor(turns)) & 7];
}

struct HudState {
    bool flying;
    float speed, viewRadius, height, yawDeg, fps;
    int tiles;
};

// The status panel: what mode you are in and what your settings are, one line each.
inline std::string hudText(const HudState& s) {
    char text[200];
    std::snprintf(text, sizeof text, "%s  %.*f m/s\nheight  %.0f m\nview    %.0f m\nfacing  %s\ntiles   %d\n%.0f fps  H help",
                  s.flying ? "flying" : "on foot", s.speed < 10 ? 1 : 0, static_cast<double>(s.speed), static_cast<double>(s.height),
                  static_cast<double>(s.viewRadius), compass(s.yawDeg), s.tiles, static_cast<double>(s.fps));
    return text;
}

}  // namespace ponderosa
