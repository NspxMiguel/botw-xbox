// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace uwp {
struct ArtImage {
    uint32_t width = 0, height = 0;
    // Straight RGBA, top left origin. No artwork is embedded in the executable.
    std::vector<uint8_t> rgba;
};
inline constexpr size_t art_byte_limit = 16 * 1024 * 1024;
inline std::optional<ArtImage> decode_tga(std::span<const uint8_t> bytes) {
    if (bytes.size() < 18 || bytes.size() > art_byte_limit || bytes[1] != 0 ||
        (bytes[2] != 2 && bytes[2] != 10) || (bytes[16] != 24 && bytes[16] != 32) ||
        (bytes[17] & 0xc0) != 0 || (bytes[17] & 15) > (bytes[16] == 32 ? 8 : 0))
        return std::nullopt;
    uint32_t width = bytes[12] | (uint32_t(bytes[13]) << 8);
    uint32_t height = bytes[14] | (uint32_t(bytes[15]) << 8);
    size_t count = size_t(width) * height, cursor = 18 + size_t(bytes[0]);
    if (!width || !height || count > art_byte_limit / 4 || cursor > bytes.size())
        return std::nullopt;
    ArtImage image{width, height, std::vector<uint8_t>(count * 4)};
    size_t pixel = 0, stride = bytes[16] / 8;
    auto emit = [&](size_t at) {
        size_t x = pixel % width, y = pixel / width;
        if (bytes[17] & 16)
            x = width - 1 - x;
        if (!(bytes[17] & 32))
            y = height - 1 - y;
        size_t out = (y * width + x) * 4;
        image.rgba[out] = bytes[at + 2];
        image.rgba[out + 1] = bytes[at + 1];
        image.rgba[out + 2] = bytes[at];
        // A 32-bit TGA with no attribute bits has an unused fourth byte.
        image.rgba[out + 3] = stride == 4 && (bytes[17] & 15) ? bytes[at + 3] : 255;
        ++pixel;
    };
    while (pixel < count) {
        size_t length = 1;
        bool repeat = false;
        if (bytes[2] == 10) {
            if (cursor == bytes.size())
                return std::nullopt;
            uint8_t packet = bytes[cursor++];
            length = (packet & 127) + 1;
            repeat = (packet & 128) != 0;
        }
        if (length > count - pixel)
            return std::nullopt;
        size_t needed = (repeat ? 1 : length) * stride;
        if (needed > bytes.size() - cursor)
            return std::nullopt;
        for (size_t i = 0; i < length; ++i)
            emit(cursor + (repeat ? 0 : i * stride));
        cursor += needed;
    }
    return image;
}
struct ArtColor {
    float r = 1, g = 1, b = 1;
};
inline double black_contrast(ArtColor color) {
    auto channel = [](double c) { return c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4); };
    return 1 + 20 * (.2126 * channel(color.r) + .7152 * channel(color.g) + .0722 * channel(color.b));
}
inline ArtColor art_accent(const ArtImage &image) {
    // Hue buckets prevent a large neutral sky/border from drowning out saturated content.
    struct Bucket {
        double weight = 0, r = 0, g = 0, b = 0;
    };
    std::array<Bucket, 36> buckets{};
    for (size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        double r = image.rgba[i] / 255., g = image.rgba[i + 1] / 255., b = image.rgba[i + 2] / 255.;
        double high = std::max({r, g, b}), low = std::min({r, g, b}), delta = high - low;
        if (high < .08 || delta / high < .25 || image.rgba[i + 3] < 128)
            continue;
        double hue = high == r ? (g - b) / delta : high == g ? 2 + (b - r) / delta : 4 + (r - g) / delta;
        if (hue < 0)
            hue += 6;
        auto &bucket = buckets[std::min(35, int(hue * 6))];
        double weight = delta * image.rgba[i + 3] / 255.;
        bucket.weight += weight;
        bucket.r += r * weight;
        bucket.g += g * weight;
        bucket.b += b * weight;
    }
    auto best = std::max_element(buckets.begin(), buckets.end(),
                                 [](auto &a, auto &b) { return a.weight < b.weight; });
    ArtColor color;
    if (best->weight)
        color = {float(best->r / best->weight), float(best->g / best->weight), float(best->b / best->weight)};
    // Mix toward white, retaining the selected hue, until WCAG AA on true black.
    const ArtColor original = color;
    for (int step = 1; black_contrast(color) < 4.5 && step <= 1000; ++step) {
        float t = step / 1000.f;
        color = {original.r + (1 - original.r) * t, original.g + (1 - original.g) * t,
                 original.b + (1 - original.b) * t};
    }
    return color;
}
struct GameArt {
    std::optional<ArtImage> tv, icon, drc;
    ArtColor accent() const {
        return tv ? art_accent(*tv) : icon ? art_accent(*icon) : drc ? art_accent(*drc) : ArtColor{};
    }
};
inline int home_focus(int focus, int direction) {
    if (focus == 0 && (direction == 0 || direction == 1))
        return 1;
    if (focus == 1 && direction == 2)
        return 2;
    if (focus == 2 && (direction == 2 || direction == 3))
        return 0;
    if (focus == 2 && direction == 0)
        return 1;
    return focus;
}
// Integral of smoothstep velocity: degrees stay unwrapped, continuous and monotonic.
inline double disc_angle(double seconds, bool loading, bool reduced_motion) {
    if (!loading || reduced_motion || !std::isfinite(seconds) || seconds <= 0)
        return 0;
    constexpr double ramp = 1.2, speed = 900;
    if (seconds >= ramp)
        return speed * (seconds - ramp / 2);
    double u = seconds / ramp;
    return speed * ramp * (u * u * u - u * u * u * u / 2);
}
} // namespace uwp
