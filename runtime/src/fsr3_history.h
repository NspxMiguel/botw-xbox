// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace fsr3 {
// A conservative image discontinuity signal when guest camera matrices are unknown.
// Small changes may reset unnecessarily; spatially identical cuts need no reset.
class History {
  public:
    static constexpr uint32_t columns = 16, rows = 9, samples = columns * rows;
    using Image = std::array<std::array<uint8_t, 3>, samples>;
    bool update(const Image &image, double frame_ms) {
        unsigned difference = 0, changed = 0;
        for (uint32_t i = 0; i < samples; ++i) {
            unsigned pixel = 0;
            for (unsigned channel = 0; channel < 3; ++channel)
                pixel += unsigned(std::abs(int(image[i][channel]) - int(previous[i][channel])));
            difference += pixel;
            changed += pixel > 48;
        }
        const bool reset = !valid || !std::isfinite(frame_ms) || frame_ms > 100 ||
                           difference > samples * 3 * 10 || changed > samples / 4;
        previous = image;
        valid = true;
        return reset;
    }
    void clear() { valid = false; }

  private:
    Image previous{};
    bool valid = false;
};
} // namespace fsr3
