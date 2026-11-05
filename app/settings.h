// What the player can change while playing. Pure: tested without a GPU.
#pragma once

#include <algorithm>
#include <cmath>

namespace ponderosa {

constexpr float kMinFlySpeed = 5.0f, kMaxFlySpeed = 800.0f;    // metres a second
constexpr float kMinWalkSpeed = 0.7f, kMaxWalkSpeed = 14.0f;
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

}  // namespace ponderosa
