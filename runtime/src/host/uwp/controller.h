// SPDX-License-Identifier: MIT
#pragma once
#include "../../input/input.h"
#include <cstdint>

namespace uwp {
// Windows.Gaming.Input.GamepadButtons ABI; no Windows dependency in mapping/tests.
enum Button : uint32_t {
    menu = 1,
    view = 2,
    a = 4,
    b = 8,
    x = 16,
    y = 32,
    up = 64,
    down = 128,
    left = 256,
    right = 512,
    lb = 1024,
    rb = 2048,
    ls = 4096,
    rs = 8192
};
struct Reading {
    uint32_t buttons = 0;
    double lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
};
struct MappedInput {
    input::PadState pad;
    bool return_to_menu = false;
    bool toggle_gamepad_screen = false;
};
class Controller {
    uint32_t previous_ = 0;
    bool blocked_ = false;

  public:
    MappedInput update(const Reading &, int layout = 0);
    // On focus loss/disconnect, require release before another UI shortcut.
    void reset() {
        previous_ = 0;
        blocked_ = true;
    }
};
// Apparatus mode consumes camera/trigger/recenter inputs while preserving movement and cancel.
void apply_motion_controls(input::PadState &pad, const Reading &reading, bool active);
// Wii U Pro Controller button ABI for a padscore adapter. Sticks use the same axes as VPAD.
uint32_t pro_buttons(uint32_t vpad_buttons);
} // namespace uwp
