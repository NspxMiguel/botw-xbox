// SPDX-License-Identifier: MIT
#include "../../sim_series_s.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <limits>
#include <thread>

void log_write(const char *, ...) {}
int main(int argc, char **argv) {
    using namespace sim_series_s;
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto inf = std::numeric_limits<double>::infinity();
    assert(factor("", 3, 10) == 3);
    for (auto text : {"nan", "inf", "0", "11", "3junk", "3 ", "1e999"})
        assert(factor(text, 3, 10) == 3);
    assert(factor("1", 3, 10) == 1);
    assert(factor("10", 3, 10) == 10);
    assert(factor("1.05", 1, 2) == 1.05);
    assert(factor("2.01", 1.05, 2) == 1.05);
    assert(extra_seconds(0.02, 3) == 0.04);
    assert(extra_seconds(5, 3) == 0.1);
    assert(extra_seconds(-1, 3) == 0);
    assert(extra_seconds(nan, 3) == 0);
    assert(extra_seconds(1, inf) == 0);
    assert(smooth_rate(0, 4096, 2048) == 2);
    assert(smooth_rate(2, 4096, 1024) == 2.5);
    assert(smooth_rate(2, 4096, 0) == 2);
    assert(smooth_rate(2, 4096, nan) == 2);
    assert(smooth_rate(2, 0, 100) == 2);
    assert(loop_count(10, 1000, 3, 0) == kBootstrapLoops);
    assert(loop_count(10, 1000, 3, 2) == 40);
    assert(loop_count(1000, 1000, 3, 2) == 200); // 100 ms target clamp
    assert(loop_count(10, 1000, 3, 1e10) == kMaxLoops);
    assert(loop_count(10, 1000, 1, 2) == 0);
    assert(loop_count(0, 1000, 3, 2) == 0);
    assert(loop_count(nan, 1000, 3, 2) == 0);
    const auto &config = settings();
    const std::string mode = argc > 1 ? argv[1] : "off";
    assert(config.enabled == (mode == "on"));
    assert(config.gpu_factor == 3);
    assert(config.cpu_factor == 1.05);
    if (config.enabled) {
        cpu_frame(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto before_stretch = std::chrono::steady_clock::now();
        cpu_frame(0);
        const auto stretch =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - before_stretch).count();
        auto sample = take_cpu_stats();
        assert(stretch >= sample.work_ms * 1e-3 * (config.cpu_factor - 1) * 0.9);
        assert(sample.frames == 1 && sample.work_ms >= 19);
        assert(take_cpu_stats().frames == 0);
        // A blocked interval contributes no work and therefore no proportional delay.
        cpu_frame(1000000000);
        sample = take_cpu_stats();
        assert(sample.frames == 1 && sample.work_ms == 0);
    } else {
        cpu_frame(0);
        assert(take_cpu_stats().frames == 0);
    }
    std::puts("Series S simulation portable tests passed");
}
