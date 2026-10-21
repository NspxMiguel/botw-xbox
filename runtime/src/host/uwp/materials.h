// SPDX-License-Identifier: MIT
#pragma once
#include "art.h"
#include "polish.h"
namespace uwp {
inline ArtColor rainbow(double h) {
    auto c = [&](double offset) {
        return float(std::clamp(std::abs(std::fmod(h * 6 + offset, 6.) - 3) - 1, 0., 1.));
    };
    h -= std::floor(h);
    return {c(0), c(4), c(2)};
}
// Procedural optical materials only; no game imagery or reference-video pixels.
inline ArtImage disc_material(bool silver) {
    ArtImage out{600, 600, std::vector<uint8_t>(600 * 600 * 4)};
    for (unsigned y = 0; y < 600; ++y)
        for (unsigned x = 0; x < 600; ++x) {
            double dx = (x + .5 - 300) / 2, dy = (y + .5 - 300) / 2, r = std::hypot(dx, dy),
                   a = std::atan2(dy, dx);
            double band = std::pow(std::cos(2 * (a - .8)), silver ? 6 : 16);
            auto color = rainbow((a - .8) / pi + r / 150 * .6);
            double base = silver ? 120 + 90 * band + 25 * r / 150 : 255;
            double mix = silver ? .45 * band : .12;
            size_t i = (y * 600 + x) * 4;
            for (int c = 0; c < 3; ++c) {
                double v = c == 0 ? color.r : c == 1 ? color.g : color.b;
                out.rgba[i + c] = uint8_t(std::clamp(base * (1 - mix) + v * 255 * mix, 0., 255.));
            }
            double edge = std::clamp((150 - r) * 2, 0., 1.) * std::clamp((r - 20) * 2, 0., 1.);
            out.rgba[i + 3] = uint8_t(255 * edge * (silver ? 1 : .30 * band * r / 150));
        }
    return out;
}
inline ArtImage silver_background() {
    ArtImage out{1280, 720, std::vector<uint8_t>(1280 * 720 * 4)};
    for (unsigned y = 0; y < 720; ++y)
        for (unsigned x = 0; x < 1280; ++x) {
            double g = double(y) / 720, u = double(x) / 1280 * 2 - 1;
            double band = std::exp(-std::pow(u * .8 + g * .6 - .3, 2) / .02);
            auto c = rainbow(u * .5 + g * .3);
            size_t i = (y * 1280 + x) * 4;
            for (int k = 0; k < 3; ++k)
                out.rgba[i + k] = uint8_t((200 - 50 * g) * (1 - .3 * band) + 255 *
                                                                                 (k == 0   ? c.r
                                                                                  : k == 1 ? c.g
                                                                                           : c.b) *
                                                                                 .3 * band);
            out.rgba[i + 3] = 255;
        }
    return out;
}
inline ArtImage ring_halo() {
    ArtImage out{360, 360, std::vector<uint8_t>(360 * 360 * 4)};
    for (unsigned y = 0; y < 360; ++y)
        for (unsigned x = 0; x < 360; ++x) {
            double d = std::hypot(x + .5 - 180, y + .5 - 180) - 155;
            size_t i = (y * 360 + x) * 4;
            out.rgba[i] = out.rgba[i + 1] = out.rgba[i + 2] = 255;
            out.rgba[i + 3] = uint8_t(110 * std::exp(-d * d / (2 * 7 * 7)));
        }
    return out;
}
} // namespace uwp
