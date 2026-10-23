// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace fsr {
// Stable persisted values reserved for w17 FSR 3.1. Resolve unsupported modes explicitly.
enum class Upscaler : int { Auto = 0, Off = 1, Spatial = 2, Temporal = 3 };
enum class FrameGeneration : int { Auto = 0, Off = 1, Fsr31 = 2 };
enum class Quality : int { Auto = 0, UltraQuality = 1, Quality = 2, Balanced = 3, Performance = 4 };
// Upscaling is opt-in: Auto (the stored default) and Off render native with no FSR.
constexpr Upscaler effective_upscaler(Upscaler requested) {
    return requested == Upscaler::Auto || requested == Upscaler::Off ? Upscaler::Off : Upscaler::Spatial;
}
constexpr FrameGeneration effective_frame_generation(FrameGeneration) { return FrameGeneration::Off; }
constexpr bool should_upscale(Upscaler requested, int iw, int ih, int ow, int oh) {
    return effective_upscaler(requested) == Upscaler::Spatial && iw > 0 && ih > 0 && ow >= iw && oh >= ih &&
           (ow > iw || oh > ih);
}
inline std::atomic<Upscaler> upscaler{Upscaler::Off};
// The player's choice as an fsr3::select override; BOTW_UPSCALER (testing) wins.
inline std::string_view upscaler_override() {
    if (const char *value = std::getenv("BOTW_UPSCALER"))
        return value;
    switch (upscaler.load()) {
    case Upscaler::Spatial:
        return "fsr1";
    case Upscaler::Temporal:
        return "fsr3";
    default:
        return "off";
    }
}
inline std::atomic<FrameGeneration> frame_generation{FrameGeneration::Off};
constexpr float ratio(Quality quality) {
    switch (quality) {
    case Quality::UltraQuality:
        return 1.3f;
    case Quality::Quality:
        return 1.5f;
    case Quality::Balanced:
        return 1.7f;
    case Quality::Performance:
        return 2.f;
    default:
        return 1.f;
    }
}
inline float select_scale(int width, int height, bool series_s, Quality quality, float requested = 0) {
    const float cap = series_s ? 1.5f : 3.f;
    const float display = std::max(1.f, std::min(width / 1280.f, height / 720.f));
    const float scale = std::isfinite(requested) && requested > 0 ? requested : display / ratio(quality);
    return std::clamp(scale, 1.f, std::min(cap, display));
}
constexpr float halton(uint32_t index, uint32_t base) {
    float value = 0, fraction = 1;
    while (index) {
        fraction /= float(base);
        value += fraction * float(index % base);
        index /= base;
    }
    return value;
}
// Pixel-space offsets for the future temporal scene projection hook; not applied to
// the spatial path. Reset the index together with temporal history on camera cuts.
constexpr std::array<float, 2> jitter(uint32_t frame, uint32_t phases = 8) {
    const uint32_t index = frame % std::max(1u, phases) + 1;
    return {halton(index, 2) - 0.5f, halton(index, 3) - 0.5f};
}
} // namespace fsr
