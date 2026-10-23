// SPDX-License-Identifier: MIT
#include "../first_frame.h"
#include "../materials.h"
#include <cstdlib>
#include <iostream>
#define CHECK(...)                                                                                           \
    do {                                                                                                     \
        if (!(__VA_ARGS__)) {                                                                                \
            std::cerr << "Failed line " << __LINE__ << '\n';                                                 \
            std::exit(1);                                                                                    \
        }                                                                                                    \
    } while (false)
int main() {
    using namespace uwp;
    // Springs converge at different refresh rates and retain the requested overshoot.
    for (int hz : {30, 60, 120}) {
        Spring s;
        double peak = 0;
        for (int i = 0; i < hz * 4; ++i) {
            s.step(22, 1. / hz);
            peak = std::max(peak, s.value);
        }
        CHECK(std::abs(s.value - 22) < .001 && peak > 23);
        for (int i = 0; i < hz * 4; ++i)
            s.step(0, 1. / hz);
        CHECK(std::abs(s.value) < .001);
    }
    Spring a, b;
    for (int i = 0; i < 120; ++i)
        a.step(1, 1. / 60);
    for (int i = 0; i < 240; ++i)
        b.step(1, 1. / 120);
    CHECK(std::abs(a.value - b.value) < 1e-12);
    auto before = launch_motion(1 - 1e-7), after = launch_motion(1 + 1e-7);
    CHECK(std::abs(before.angle - after.angle) < .001);
    CHECK(launch_motion(.1).z < 0 && launch_motion(3.2).z == 60);
    CHECK(launch_motion(2.2).fade == 0 && launch_motion(3.2).fade == 1);
    CHECK(std::abs((launch_motion(4.001).angle - launch_motion(4).angle) / .001 + 792) < 1e-6);
    auto faces = triforce_geometry();
    float min = 9999, max = 0, area = 0;
    for (auto t : faces) {
        for (auto p : t) {
            min = std::min(min, p.x);
            max = std::max(max, p.x);
        }
        area += std::abs((t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[2].x - t[0].x) * (t[1].y - t[0].y)) / 2;
    }
    CHECK(max - min == 66 && (min + max) / 2 == 1200 && std::abs(area - 1414.698) < .1);
    std::vector<float> samples(48000 * 12);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = float(std::sin(i * .01));
    auto original = samples;
    size_t start = prepare_drive_loop(samples);
    CHECK(start == 420000);
    CHECK(samples[samples.size() - 4800] == original[original.size() - 4800]);
    CHECK(std::abs(samples.back() - original[start - 1]) < 1e-6);
    CHECK(std::abs(samples.back() - samples[start]) < .011);
    std::vector<float> empty;
    CHECK(prepare_drive_loop(empty) == 0);
    CHECK(drive_fade(0) == 1 && drive_fade(.25) == 0 && drive_fade(.125) == .5f);
    auto sheen = disc_material(false), silver = disc_material(true), halo = ring_halo();
    CHECK(sheen.rgba[(300 * 600 + 300) * 4 + 3] == 0);
    CHECK(silver.rgba[(300 * 600 + 100) * 4 + 3] == 255);
    CHECK(halo.rgba[(180 * 360 + 335) * 4 + 3] > 100);
    CHECK(!first_frame_presented.load());
    signal_first_frame();
    CHECK(first_frame_presented.load());
    std::cout << "PASS: springs across 30/60/120 Hz, launch continuity, triforce geometry, metadata regions, "
                 "100 ms loop seam, 250 ms fade, optical masks and first-frame signal\n";
}
