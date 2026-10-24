// SPDX-License-Identifier: MIT
#pragma once
#include "../../fsr3_policy.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace uwp {
// DXGI publishes actual buffer dimensions; the Latte compositor reads them on
// its own thread. CoreWindow can remain 1080p while these buffers are 4K.
class OutputExtent {
  public:
    void publish(int width, int height) {
        if (width > 0 && height > 0)
            pixels.store((uint64_t(uint32_t(width)) << 32) | uint32_t(height));
    }
    bool read(int &width, int &height) const {
        const auto value = pixels.load();
        if (!value)
            return false;
        width = int(value >> 32);
        height = int(uint32_t(value));
        return true;
    }

  private:
    std::atomic<uint64_t> pixels{0};
};
inline OutputExtent game_output_extent;

struct OutputMode {
    int width = 0, height = 0;
    double hz = 0;
    bool sdr = true, stereo = false;
};
// Prefer resolution for this 60 fps game. A 4K30 video mode must never replace
// a 1080p60 game mode; 4K60 may replace 1080p120 (frame generation is opt-in).
constexpr bool supported_game_mode(OutputMode mode, fsr3::Console console) {
    const bool limited = console == fsr3::Console::SeriesS;
    return mode.width > 0 && mode.width <= (limited ? 2560 : 3840) && mode.height > 0 &&
           mode.height <= (limited ? 1440 : 2160) && mode.hz >= 59 && mode.sdr && !mode.stereo;
}
constexpr bool better_game_mode(OutputMode candidate, OutputMode current,
                                fsr3::Console console = fsr3::Console::Desktop) {
    return supported_game_mode(candidate, console) &&
           (!supported_game_mode(current, console) || candidate.height > current.height);
}
inline void cap_output_extent(int &width, int &height, fsr3::Console console) {
    const bool limited = console == fsr3::Console::SeriesS;
    const double scale = std::min({1., (limited ? 2560. : 3840.) / std::max(1, width),
                                   (limited ? 1440. : 2160.) / std::max(1, height)});
    width = std::max(1, int(width * scale));
    height = std::max(1, int(height * scale));
}
inline float native_render_scale(int width, int height, fsr3::Console console) {
    const float display = std::min(width / 1280.f, height / 720.f);
    // 4K output is independent of internal targets. The 5 GiB process budget
    // leaves insufficient reserve for unmeasured 2160p targets; see remaster-4k.md.
    // Series S starts at 900p to reduce target memory and GPU work. This is a
    // conservative, unmeasured default, not a claim of 60 fps on that hardware.
    const float cap = console == fsr3::Console::SeriesS   ? 1.25f
                      : console == fsr3::Console::SeriesX ? 2.f
                                                          : 3.f;
    return std::clamp(display, 1.f, cap);
}
} // namespace uwp
