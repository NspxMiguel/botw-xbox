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
struct DiscIdentity {
    std::string id, region;
};
inline std::optional<DiscIdentity> disc_identity(std::string_view xml) {
    auto field = [&](std::string_view key) -> std::string {
        auto start = xml.find("<" + std::string(key));
        if (start == xml.npos)
            return {};
        auto end = xml.find('>', start);
        if (end == xml.npos)
            return {};
        auto close = xml.find("</" + std::string(key) + ">", end);
        if (close == xml.npos)
            return {};
        auto s = xml.substr(end + 1, close - end - 1);
        auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == s.npos ? "" : std::string(s.substr(a, b - a + 1));
    };
    auto product = field("product_code"), company = field("company_code");
    if (product.size() >= 4)
        product = product.substr(product.size() - 4);
    if (company.size() >= 2)
        company = company.substr(company.size() - 2);
    auto valid = [](std::string const &s) {
        return std::all_of(s.begin(), s.end(),
                           [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
    };
    if (product.size() != 4 || company.size() != 2 || !valid(product) || !valid(company))
        return {};
    std::string region = product[3] == 'E' ? "US" : product[3] == 'P' ? "EN" : product[3] == 'J' ? "JA" : "";
    if (region.empty())
        return {};
    return DiscIdentity{product + company, region};
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
