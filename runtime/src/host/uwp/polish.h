// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace uwp {
inline constexpr double pi = 3.141592653589793;
inline double ease(double t) {
    t = std::clamp(t, 0., 1.);
    return t * t * (3 - 2 * t);
}
struct Spring {
    double value = 0, velocity = 0;
    void step(double goal, double dt, double frequency = 10, double damping = .45) {
        if (!std::isfinite(goal) || !std::isfinite(dt) || dt <= 0)
            return;
        // Exact damped oscillator solution, stable through frame stalls and refresh changes.
        dt = std::min(dt, .1);
        double x = value - goal, w = frequency * std::sqrt(1 - damping * damping);
        double b = (velocity + damping * frequency * x) / w;
        double c = std::cos(w * dt), s = std::sin(w * dt), decay = std::exp(-damping * frequency * dt);
        value = goal + decay * (x * c + b * s);
        velocity = decay * ((-x * w * s + b * w * c) - damping * frequency * (x * c + b * s));
    }
};
struct LaunchMotion {
    double angle, z, fade;
};
inline LaunchMotion launch_motion(double t) {
    t = std::max(0., t);
    // 1.1 revolutions in the first second, reaching 2.2 rev/s without a velocity jump.
    return {-360 * (1.1 * std::pow(std::min(t, 1.), 2) + 2.2 * std::max(0., t - 1)),
            -18 * std::sin(pi * std::clamp(t / .2, 0., 1.)) + 60 * ease((t - 1.2)), ease(t - 2.2)};
}
struct Point {
    float x, y;
};
using Triangle = std::array<Point, 3>;
inline std::array<Triangle, 3> triforce_geometry() {
    constexpr float x = 1200, y = 640, w = 66, h = w * .8660254f;
    return {{{{{x, y - h / 2}, {x - w / 4, y}, {x + w / 4, y}}},
             {{{x - w / 4, y}, {x - w / 2, y + h / 2}, {x, y + h / 2}}},
             {{{x + w / 4, y}, {x, y + h / 2}, {x + w / 2, y + h / 2}}}}};
}
// The packaged WAV has already had 350 ms removed. Blend its tail into the steady loop head.
inline size_t prepare_drive_loop(std::vector<float> &samples, unsigned rate = 48000) {
    size_t start = size_t(8.65 * rate), overlap = rate / 10;
    if (samples.size() < start + overlap * 2)
        return 0;
    size_t end = samples.size() - overlap;
    for (size_t i = 0; i < overlap; ++i) {
        float t = float(ease(double(i) / double(overlap - 1)));
        samples[end + i] = samples[end + i] * (1 - t) + samples[start + i] * t;
    }
    return start + overlap;
}
inline float drive_fade(double since_frame) { return float(1 - ease(since_frame / .25)); }
} // namespace uwp
