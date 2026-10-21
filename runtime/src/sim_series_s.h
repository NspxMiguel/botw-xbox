// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace sim_series_s {
struct Config {
    bool enabled = false;
    double gpu_factor = 3.0;
    double cpu_factor = 1.05;
};
inline double factor(std::string_view text, double fallback, double maximum) {
    std::string value(text);
    char *end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    return end != value.c_str() && *end == '\0' && std::isfinite(parsed) && parsed >= 1 && parsed <= maximum
               ? parsed
               : fallback;
}
inline double smooth_rate(double previous, uint32_t loops, double ticks) {
    if (!loops || !std::isfinite(ticks) || ticks <= 0)
        return previous;
    const double measured = loops / ticks;
    return previous > 0 ? previous * 0.75 + measured * 0.25 : measured;
}
constexpr uint32_t kBootstrapLoops = 4096;
constexpr uint32_t kMaxLoops = 4194304;
constexpr double kMaxExtraSeconds = 0.1;
inline double extra_seconds(double work, double multiplier) {
    if (!std::isfinite(work) || !std::isfinite(multiplier))
        return 0;
    return std::clamp(work * (multiplier - 1), 0.0, kMaxExtraSeconds);
}
// Mirrors the shader's loop sizing; timestamps stay on the GPU for current-frame sizing.
inline uint32_t loop_count(double ticks, double frequency, double multiplier, double rate) {
    if (!std::isfinite(ticks) || !std::isfinite(frequency) || !std::isfinite(multiplier) || multiplier <= 1 ||
        ticks <= 0 || frequency <= 0)
        return 0;
    if (!std::isfinite(rate) || rate <= 0)
        return kBootstrapLoops;
    const double target = extra_seconds(ticks / frequency, multiplier) * frequency;
    return static_cast<uint32_t>(std::clamp(target * rate, 1.0, double(kMaxLoops)));
}
const Config &settings();
// Called only on the main guest thread, at the existing engine frame boundary.
void cpu_frame(uint64_t waited_ns);
struct CpuStats {
    double work_ms;
    uint64_t frames;
};
CpuStats take_cpu_stats();
} // namespace sim_series_s
