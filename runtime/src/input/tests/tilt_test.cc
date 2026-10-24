// SPDX-License-Identifier: MIT
#include "input/input.h"
#include <cassert>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <limits>

int main() {
    using namespace input;
    uint64_t t = 100000000000ull;
    auto sample = [&](float x, float y, float z = 0.f, bool center = false) {
        assert(virtual_tilt(x, y, t, z, center));
        t += 16666667;
        Motion m{};
        assert(motion(&m));
        for (float v : m.dir)
            assert(std::isfinite(v));
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) {
                float dot = 0;
                for (int k = 0; k < 3; ++k)
                    dot += m.dir[3 * a + k] * m.dir[3 * b + k];
                assert(std::abs(dot - (a == b ? 1.f : 0.f)) < 1e-5);
            }
        return m;
    };
    motion_reset();
    set_tilt_mode(false);
    auto m = sample(1, 1);
    assert(m.acc[1] == -1 && m.acc_magnitude == 1);
    assert(m.dir[0] == 1 && std::abs(m.dir[4] - 1) < 1e-6 && std::abs(m.dir[8] - 1) < 1e-6);
    for (float v : m.gyro_change)
        assert(v == 0);
    set_tilt_mode(true);
    sample(0, 1);
    for (int i = 0; i < 60; ++i)
        m = sample(0, 1);
    assert(std::abs(m.acc[2]) > .99f && std::abs(m.acc[1]) < .01f);
    assert(std::abs(m.orientation[0] - .25f) < .001f && m.gyro_change[0] == .25f);
    auto held = m;
    for (int i = 0; i < 120; ++i)
        m = sample(0, 0);
    for (int i = 0; i < 9; ++i)
        assert(std::abs(m.dir[i] - held.dir[i]) < 1e-6);
    for (float v : m.gyro_change)
        assert(v == 0);
    // Full rotations, including upside-down apparatuses and the third/yaw axis.
    for (int axis = 0; axis < 3; ++axis) {
        sample(0, 0, 0, true);
        for (int i = 0; i < 120; ++i)
            m = sample(axis == 2, axis == 0, axis == 1);
        assert(std::abs(m.orientation[axis] - .5f) < .001f);
        for (int i = 0; i < 120; ++i)
            m = sample(axis == 2, axis == 0, axis == 1);
        assert(std::abs(m.orientation[axis] - 1.f) < .001f);
        assert(std::abs(m.dir[0] - 1) < .001f && std::abs(m.dir[4] - 1) < .001f);
    }
    m = sample(1, 1, 1, true);
    assert(m.acc[1] == -1);
    for (float v : m.orientation)
        assert(v == 0);
    auto rest = sample(.1f, -.1f, .1f);
    for (float v : rest.gyro_change)
        assert(v == 0);
    sample(0, 0, 0, true);
    for (int i = 0; i < 60; ++i)
        m = sample(1, 0);
    assert(std::abs(m.acc[0]) > .99f && std::abs(m.acc[1]) < .01f);
    sample(0, 0, 0, true);
    for (int i = 0; i < 60; ++i)
        m = sample(0, 0, 1);
    assert(std::abs(m.acc[0]) < 1e-6 && m.acc[1] == -1 && std::abs(m.acc[2]) < 1e-6);
    sample(0, 0, 0, true);
    m = sample(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity());
    for (float v : m.gyro_change)
        assert(v == 0);
    // A long pause or non-monotonic sample cannot cause a sudden multi-turn jump.
    t += 20000000000ull;
    m = sample(0, 1);
    assert(m.orientation[0] <= .0251f);
    t -= 1000000000ull;
    auto before = m;
    m = sample(0, 1);
    assert(m.orientation[0] == before.orientation[0]);
    set_tilt_mode(false);
    m = sample(1, 1);
    assert(m.acc[1] == -1 && m.orientation[0] == 0);
    const float acc[3] = {0, 9.81f, 0}, gyro[3] = {0, 0, 0};
    const auto host_now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                    std::chrono::steady_clock::now().time_since_epoch())
                                                    .count());
    motion_sensor(false, acc, 1234567); // SDL timestamps deliberately use a different epoch.
    motion_sensor(true, gyro, 1234567);
    assert(!virtual_tilt(0, 1, host_now));
    t = host_now + 600000000;
    sample(0, 0); // Sensor disconnect falls back without a process restart.
    motion_reset();
    sample(0, 0); // Reset clears real-source ownership and virtual timestamps.
    std::puts("PASS: virtual three-axis pose, hold/recenter, finite inputs, "
              "timing, sensor handoff");
}
