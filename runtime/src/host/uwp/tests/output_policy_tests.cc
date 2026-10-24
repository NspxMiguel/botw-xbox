// SPDX-License-Identifier: MIT
#include "../output_policy.h"
#include <cassert>

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
    assert(native_render_scale(3840, 2160, Console::SeriesS) == 1.5f);
    assert(native_render_scale(3840, 2160, Console::Desktop) == 3.f);
}
