// SPDX-License-Identifier: MIT
// Exercise the guest ABI using synthetic memory; no game executable is
// required.
#include "hle.h"
#include "input/input.h"
#include <cassert>
#include <cmath>
#include <cstdio>

extern "C" {
uint8_t *g_mem = nullptr;
void hle_vpad_VPADRead(PPCContext *);
void hle_vpad_VPADGetTPCalibratedPoint(PPCContext *);
}
HleRegistration::HleRegistration(const char *, const char *, GuestFn) {}

int main() {
    uint8_t memory[4096]{};
    g_mem = memory;
    PPCContext c{};
    auto read = [&](uint32_t channel = 0) {
        c.r[3] = channel;
        c.r[4] = 128;
        c.r[5] = 1;
        c.r[6] = 512;
        hle_vpad_VPADRead(&c);
    };
    input::PadState pad;
    pad.buttons = input::BTN_A;
    pad.touch = true;
    input::set_gamepad(pad);
    read();
    assert(c.r[3] == 1 && mem::r32(512) == 0);
    assert(mem::r32(128) == input::BTN_A && mem::r32(132) == input::BTN_A);
    for (uint32_t offset : {0x52u, 0x5Au, 0x62u}) {
        assert(mem::r16(128 + offset) == 2048 && mem::r16(130 + offset) == 2048);
        assert(mem::r16(132 + offset) == 1 && mem::r16(134 + offset) == 0);
        c.r[3] = 0;
        c.r[4] = 768;
        c.r[5] = 128 + offset;
        hle_vpad_VPADGetTPCalibratedPoint(&c);
        assert(mem::r16(768) == 640 && mem::r16(770) == 360);
        assert(mem::r16(772) == 1 && mem::r16(774) == 0);
    }
    pad.touch = false;
    pad.buttons = 0;
    input::set_gamepad(pad);
    read();
    assert(mem::r32(136) == input::BTN_A);
    for (uint32_t offset : {0x52u, 0x5Au, 0x62u})
        assert(mem::r16(132 + offset) == 0);
    // In-place calibration must preserve touch/validity, including extreme
    // coordinates.
    mem::w16(768, 65535);
    mem::w16(770, 65535);
    mem::w16(772, 1);
    mem::w16(774, 2);
    c.r[4] = c.r[5] = 768;
    hle_vpad_VPADGetTPCalibratedPoint(&c);
    assert(mem::r16(768) == 1279 && mem::r16(770) == 719);
    assert(mem::r16(772) == 1 && mem::r16(774) == 2);
    input::set_tilt_mode(true);
    pad.virtual_motion = true;
    pad.tilt_x = 1;
    input::set_gamepad(pad);
    read();
    read();
    assert(mem::rf32(128 + 0x38) == .25f); // pitch rate reaches VPAD, not the camera stick
    assert(mem::rf32(128 + 0x14) == 0);
    assert(std::abs(mem::rf32(128 + 0x28) - 1) < 1e-6);
    assert(std::abs(mem::rf32(128 + 0x30) - std::hypot(mem::rf32(128 + 0x1C), mem::rf32(128 + 0x20))) < 1e-6);
    read(1);
    assert(c.r[3] == 0 && mem::r32(512) == uint32_t(-2));
    std::puts("PASS: VPAD buttons, raw/filtered touch, calibration and virtual "
              "motion ABI");
}
