// SPDX-License-Identifier: MIT
#include "../../fsr.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(#x);                                                                    \
    } while (false)
int main() {
    using namespace fsr;
    CHECK(select_scale(3840, 2160, true, Quality::Auto) == 1.5f);
    CHECK(select_scale(3840, 2160, false, Quality::Auto) == 3.f);
    CHECK(select_scale(2560, 1440, true, Quality::Auto) == 1.5f);
    CHECK(select_scale(1920, 1080, false, Quality::Auto) == 1.5f);
    CHECK(select_scale(0, 0, false, Quality::Performance) == 1.f);
    for (int i = 1; i <= 4; ++i)
        CHECK(std::abs(select_scale(3840, 2160, false, Quality(i)) - 3.f / ratio(Quality(i))) < 0.0001f);
    CHECK(select_scale(3840, 2160, true, Quality::Auto, 100) == 1.5f);
    CHECK(select_scale(3840, 2160, false, Quality::Auto, std::numeric_limits<float>::quiet_NaN()) == 3.f);
    CHECK(effective_upscaler(Upscaler::Auto) == Upscaler::Off);
    CHECK(effective_upscaler(Upscaler::Temporal) == Upscaler::Spatial);
    CHECK(effective_upscaler(Upscaler::Off) == Upscaler::Off);
    for (auto mode : {FrameGeneration::Auto, FrameGeneration::Off, FrameGeneration::Fsr31})
        CHECK(effective_frame_generation(mode) == FrameGeneration::Off);
    CHECK(!should_upscale(Upscaler::Auto, 1920, 1080, 1920, 1080));
    CHECK(!should_upscale(Upscaler::Off, 1280, 720, 1920, 1080));
    CHECK(!should_upscale(Upscaler::Spatial, 1920, 1080, 1280, 720));
    CHECK(!should_upscale(Upscaler::Spatial, 1920, 720, 1280, 1080));
    CHECK(!should_upscale(Upscaler::Spatial, 0, 720, 1920, 1080));
    CHECK(!should_upscale(Upscaler::Auto, 1280, 720, 1920, 1080));
    upscaler = Upscaler::Auto;
    CHECK(upscaler_override() == "off");
    upscaler = Upscaler::Spatial;
    CHECK(upscaler_override() == "fsr1");
    upscaler = Upscaler::Temporal;
    CHECK(upscaler_override() == "fsr3");
    CHECK(should_upscale(Upscaler::Temporal, 1280, 720, 1920, 1080));
    CHECK(should_upscale(Upscaler::Spatial, 1280, 720, 1920, 720));
    CHECK(should_upscale(Upscaler::Spatial, 1280, 720, 1280, 1080));
    CHECK(jitter(0)[0] == 0);
    CHECK(std::abs(jitter(0)[1] + 1.f / 6.f) < 0.00001f);
    CHECK(jitter(0) == jitter(8));
    CHECK(jitter(0, 0) == jitter(1, 0));
    for (uint32_t i = 0; i < 10000; ++i)
        for (auto v : jitter(i, 32))
            CHECK(v >= -0.5f && v < 0.5f);
    std::cout << "FSR portable tests: PASS (console caps, presets, output dimensions, explicit fallbacks, "
                 "jitter)\n";
}
