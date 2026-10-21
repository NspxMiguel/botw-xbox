// SPDX-License-Identifier: MIT
#include "../../fsr3_history.h"
#include "../../fsr3_policy.h"
#include <cassert>
int main() {
    using namespace fsr3;
    for (auto console : {Console::Desktop, Console::SeriesX, Console::SeriesS}) {
        for (bool sim : {false, true}) {
            for (bool sdk : {false, true}) {
                for (bool dll : {false, true}) {
                    for (auto value : {"", "fsr3", "fsr1", "off", "invalid"}) {
                        const auto result = select(console, sim, value, sdk, dll);
                        const auto expected = std::string_view(value) == "off" ? Path::Off
                                              : std::string_view(value) == "fsr1" || !sdk || !dll
                                                  ? Path::Fsr1
                                                  : Path::Fsr31;
                        assert(result.path == expected);
                        assert(result.balanced == (sim || console == Console::SeriesS));
                        assert(result.output_height == (result.balanced ? 1440 : 2160));
                        assert(select(console, sim, value, sdk, dll, 1080).output_height == 1080);
                    }
                }
            }
        }
    }
    History history;
    History::Image image{};
    assert(history.update(image, 16.67));
    assert(!history.update(image, 16.67));
    for (auto &pixel : image)
        pixel = {255, 255, 255};
    assert(history.update(image, 16.67));
    assert(!history.update(image, 16.67));
    assert(history.update(image, 500));
    history.clear();
    assert(history.update(image, 16.67));
    FrameGenerationPolicy policy;
    double now = 0;
    auto tick = [&](double ms, double hz = 60, bool paused = false) {
        now += ms;
        return policy.update(ms, hz, paused, now);
    };
    // Wall-clock jitter around a vsync-locked 60 must never enable FG.
    for (int i = 0; i < 600; ++i)
        assert(!tick(i % 2 ? 16.9 : 16.4));
    // Thresholds are strict, and the complete 30-frame window is required.
    for (int i = 0; i < 120; ++i)
        assert(!tick(18.0));
    policy = {};
    for (int i = 0; i < 29; ++i)
        assert(!tick(19));
    assert(tick(19));
    // 120 fast frames alone aren't enough until the two-second cooldown expires.
    for (int i = 0; i < 120; ++i)
        assert(tick(15));
    for (int i = 0; i < 13; ++i)
        assert(tick(15));
    assert(!tick(15));
    // A fresh enable window cannot bypass the same cooldown.
    for (int i = 0; i < 105; ++i)
        assert(!tick(19));
    assert(tick(19));
    // Dead band retains the state, including exactly 15.5 ms over 120 frames.
    for (int i = 0; i < 240; ++i)
        assert(tick(15.5));
    assert(!tick(14));
    policy = {};
    for (int i = 0; i < 29; ++i)
        assert(!tick(19));
    assert(tick(19));
    now += 2000;
    for (int i = 0; i < 119; ++i)
        assert(tick(14));
    assert(!tick(14));
    // Average, not a run of individually slow/fast samples.
    policy = {};
    for (int i = 0; i < 29; ++i)
        assert(!tick(i % 2 ? 17 : 21));
    assert(tick(17));
    assert(!tick(33.33, 0));
    assert(!tick(33.33, 60, true));
    assert(!policy.update(std::numeric_limits<double>::quiet_NaN(), 60, false, now));
    policy = {};
    for (int i = 0; i < 120; ++i)
        assert(!tick(16, 120));
    policy = {};
    for (int i = 0; i < 29; ++i) {
        now += 33.33;
        assert(!policy.update(33.33, 60, false, now));
    }
    now += 33.33;
    assert(policy.update(33.33, 60, false, now));
}
