// SPDX-License-Identifier: MIT
#include "../../../fsr.h"
#include "../console_policy.h"
#include "../output_policy.h"
#include <cassert>
#include <cstring>

int main() {
    using namespace uwp;
    using fsr3::Console;
    OutputExtent extent;
    int width = 1920, height = 1080; // Xbox's logical CoreWindow size.
    assert(!extent.read(width, height));
    assert(width == 1920 && height == 1080);
    extent.publish(3840, 2160);
    assert(extent.read(width, height));
    assert(width == 3840 && height == 2160); // The final viewport covers the backbuffer.
    extent.publish(0, 0);                    // A minimized/invalid size cannot replace valid output.
    assert(extent.read(width, height) && width == 3840 && height == 2160);
    extent.publish(1920, 1080);
    assert(extent.read(width, height) && width == 1920 && height == 1080);
    const OutputMode hd{1920, 1080, 60};
    assert(better_game_mode({3840, 2160, 60}, hd));
    assert(better_game_mode({3840, 2160, 60}, {3840, 2160, 30}));
    assert(!better_game_mode({3840, 2160, 30}, hd));
    assert(better_game_mode({3840, 2160, 60}, {1920, 1080, 120}));
    assert(better_game_mode({3840, 2160, 119.88}, {1920, 1080, 120}));
    assert(!better_game_mode({3840, 2160, 60, false}, hd));
    assert(!better_game_mode({3840, 2160, 60, true, true}, hd));
    assert(!better_game_mode({7680, 4320, 60}, hd));
    assert(!better_game_mode(hd, hd));
    assert(native_render_scale(1920, 1080, Console::SeriesX) == 1.5f);
    assert(native_render_scale(3840, 2160, Console::SeriesX) == 2.f);
    assert(native_render_scale(2560, 1440, Console::SeriesX) == 2.f);
    assert(native_render_scale(3840, 2160, Console::SeriesS) == 1.25f);
    assert(native_render_scale(3840, 2160, Console::Desktop) == 3.f);

    assert(select_console_profile(ConsoleModel::SeriesS, false) == Console::SeriesS);
    assert(select_console_profile(ConsoleModel::SeriesX, false) == Console::SeriesX);
    assert(select_console_profile(ConsoleModel::UnknownXbox, false) == Console::SeriesS);
    assert(select_console_profile(ConsoleModel::Desktop, false) == Console::Desktop);
    assert(select_console_profile(ConsoleModel::SeriesX, true) == Console::SeriesS);
    assert(select_console_profile(ConsoleModel::Desktop, true) == Console::SeriesS);
    assert(std::strcmp(console_model_name(ConsoleModel::SeriesS), "Xbox Series S") == 0);

    const OutputMode qhd{2560, 1440, 60};
    const OutputMode uhd{3840, 2160, 60};
    assert(better_game_mode(qhd, uhd, Console::SeriesS)); // Leave an over-budget HDMI mode.
    assert(better_game_mode(hd, uhd, Console::SeriesS));  // 1080p-only fallback.
    assert(better_game_mode(qhd, hd, Console::SeriesS));
    assert(!better_game_mode(uhd, hd, Console::SeriesS));
    assert(!better_game_mode({2560, 1440, 30}, hd, Console::SeriesS));
    assert(!better_game_mode({2560, 1440, 60, false}, hd, Console::SeriesS));
    assert(!better_game_mode({2560, 1440, 60, true, true}, hd, Console::SeriesS));
    assert(!better_game_mode({3440, 1440, 60}, hd, Console::SeriesS));
    assert(!better_game_mode(hd, hd, Console::SeriesS));
    assert(better_game_mode(uhd, qhd, Console::SeriesX));

    // Automatic output/native-render defaults, including rejected HDMI requests.
    for (auto model : {ConsoleModel::SeriesS, ConsoleModel::UnknownXbox}) {
        const auto console = select_console_profile(model, false);
        for (const auto mode : {OutputMode{1280, 720, 60}, hd, qhd, uhd}) {
            width = mode.width;
            height = mode.height;
            cap_output_extent(width, height, console);
            assert(width <= 2560 && height <= 1440);
            const float scale = native_render_scale(width, height, console);
            assert(scale == (height == 720 ? 1.f : 1.25f));
            assert(1280 * scale <= width && 720 * scale <= height);
            // Auto and Off retain native rendering; opt-in FSR still works.
            assert(!fsr::should_upscale(fsr::Upscaler::Auto, int(1280 * scale), int(720 * scale), width,
                                        height));
            assert(
                !fsr::should_upscale(fsr::Upscaler::Off, int(1280 * scale), int(720 * scale), width, height));
            if (height > 900)
                assert(fsr::should_upscale(fsr::Upscaler::Spatial, 1600, 900, width, height));
        }
    }
    width = 3440;
    height = 1440;
    cap_output_extent(width, height, Console::SeriesS);
    assert(width == 2560 && height <= 1440);
    width = height = 0;
    cap_output_extent(width, height, Console::SeriesS);
    assert(width == 1 && height == 1);
}
