// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <string_view>

namespace fsr3 {
enum class Console { Desktop, SeriesX, SeriesS };
enum class Path { Off, Fsr1, Fsr31 };
struct Selection {
    Path path;
    bool balanced;
    int output_height;
    float ratio;
};
constexpr Selection select(Console console, bool simulate_s, std::string_view override_value,
                           bool sdk_available, bool dll_available, int display_height = 2160) {
    const bool limited_console = simulate_s || console == Console::SeriesS;
    Path path = sdk_available && dll_available ? Path::Fsr31 : Path::Fsr1;
    if (override_value == "off")
        path = Path::Off;
    else if (override_value == "fsr1")
        path = Path::Fsr1;
    // An explicit fsr3 request still falls back if the optional runtime is
    // unavailable.
    return {path, limited_console, std::min(display_height, limited_console ? 1440 : 2160),
            limited_console ? 1.7f : 1.5f};
}
// Hysteresis uses real-frame timing, independently of the requested guest cap.
class FrameGenerationPolicy {
  public:
    bool update(double real_frame_ms, double mode_hz, bool paused, double now_ms) {
        if (paused || !std::isfinite(mode_hz) || !std::isfinite(real_frame_ms) || !std::isfinite(now_ms) ||
            real_frame_ms <= 0 || mode_hz < 59.0) {
            // Safety/pause overrides don't change the hysteresis latch or cooldown.
            clear_samples();
            return false;
        }
        samples[next] = real_frame_ms;
        next = (next + 1) % samples.size();
        count = std::min(count + 1, samples.size());
        bool wanted = enabled;
        if (!enabled && count >= 30 && average(30) > 18.0)
            wanted = true;
        else if (enabled && count >= 120 && average(120) < 15.5)
            wanted = false;
        if (wanted != enabled && now_ms - last_toggle_ms >= 2000.0) {
            enabled = wanted;
            last_toggle_ms = now_ms;
            clear_samples();
        }
        return enabled;
    }

  private:
    void clear_samples() { next = count = 0; }
    double average(size_t frames) const {
        double sum = 0;
        for (size_t i = 0; i < frames; ++i)
            sum += samples[(next + samples.size() - 1 - i) % samples.size()];
        return sum / double(frames);
    }
    std::array<double, 120> samples{};
    size_t next = 0, count = 0;
    bool enabled = false;
    double last_toggle_ms = -std::numeric_limits<double>::infinity();
};
inline std::atomic<Console> console{Console::Desktop};
inline std::atomic<double> display_hz{0};
inline std::atomic<int> guest_clock_fps{60};
} // namespace fsr3
