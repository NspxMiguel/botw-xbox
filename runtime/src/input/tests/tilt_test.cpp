// Virtual tilt: full right stick up for one second tilts the virtual GamePad; release eases it back flat.
// c++ -std=c++20 -Iruntime/src runtime/src/input/tests/tilt_test.cpp runtime/src/input/motion.cpp && ./a.out
#include <cassert>
#include <cmath>
#include <cstdio>
#include "input/input.h"

int main() {
    input::set_tilt_mode(true);
    input::Motion m{};
    uint64_t t = 1000000000;
    for (int i = 0; i < 60; i++, t += 16666667) assert(input::virtual_tilt(0, 1, t));
    assert(input::motion(&m));
    float tilted = m.acc[2];
    for (int i = 0; i < 120; i++, t += 16666667) input::virtual_tilt(0, 0, t);
    input::motion(&m);
    float flat = m.acc[2];
    std::printf("tilted acc.z %.3f, released acc.z %.3f, acc.y %.3f\n", tilted, flat, m.acc[1]);
    assert(std::fabs(tilted) > 0.5f && std::fabs(flat) < 0.05f && m.acc[1] < -0.95f);
    input::set_tilt_mode(false);
    input::motion_sensor(false, (const float[]){0, 9.81f, 0}, t);  // a real sensor takes over
    assert(!input::virtual_tilt(0, 1, t + 1));
    std::puts("tilt ok");
}
