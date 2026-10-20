// SPDX-License-Identifier: MIT
#include "controller.h"
#include <algorithm>
#include <cmath>

namespace uwp {
namespace {
void stick(double x, double y, float &ox, float &oy) {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        ox = oy = 0;
        return;
    }
    x = std::clamp(x, -1.0, 1.0);
    y = std::clamp(y, -1.0, 1.0);
    const double length = std::hypot(x, y);
    constexpr double deadzone = 0.18;
    if (length <= deadzone) {
        ox = oy = 0;
        return;
    }
    const double gain = (std::min(length, 1.0) - deadzone) / ((1.0 - deadzone) * length);
    ox = static_cast<float>(x * gain);
    oy = static_cast<float>(y * gain);
}
} // namespace
MappedInput Controller::update(const Reading &r, int layout) {
    MappedInput out;
    constexpr uint32_t chord = view | menu;
    const bool home = (r.buttons & chord) == chord;
    const bool screen = (r.buttons & (view | y)) == (view | y);
    const bool was_home = (previous_ & chord) == chord;
    const bool was_screen = (previous_ & (view | y)) == (view | y);
    if (r.buttons == 0)
        blocked_ = false;
    out.return_to_menu = home && !was_home && !blocked_;
    out.toggle_gamepad_screen = screen && !was_screen && !home && !blocked_;
    previous_ = r.buttons;
    if (home || screen)
        blocked_ = true;
    // Consume host shortcuts; never deliver Plus/Minus/Y or stick motion with a host chord.
    if (home || screen || blocked_)
        return out;
    const struct {
        uint32_t host, guest;
    } pairs[] = {{menu, input::BTN_PLUS},
                 {view, input::BTN_MINUS},
                 {a, layout == 1 ? input::BTN_A : input::BTN_B},
                 {b, layout == 1 ? input::BTN_B : input::BTN_A},
                 {x, layout == 1 ? input::BTN_X : input::BTN_Y},
                 {y, layout == 1 ? input::BTN_Y : input::BTN_X},
                 {up, input::BTN_UP},
                 {down, input::BTN_DOWN},
                 {left, input::BTN_LEFT},
                 {right, input::BTN_RIGHT},
                 {lb, input::BTN_L},
                 {rb, input::BTN_R},
                 {ls, input::BTN_STICK_L},
                 {rs, input::BTN_STICK_R}};
    for (auto pair : pairs)
        if (r.buttons & pair.host)
            out.pad.buttons |= pair.guest;
    if (r.lt > 0.5)
        out.pad.buttons |= input::BTN_ZL;
    if (r.rt > 0.5)
        out.pad.buttons |= input::BTN_ZR;
    stick(r.lx, r.ly, out.pad.lx, out.pad.ly);
    stick(r.rx, r.ry, out.pad.rx, out.pad.ry);
    return out;
}
uint32_t pro_buttons(uint32_t v) {
    const struct {
        uint32_t vpad, pro;
    } pairs[] = {{input::BTN_UP, 0x1},          {input::BTN_LEFT, 0x2},       {input::BTN_ZR, 0x4},
                 {input::BTN_X, 0x8},           {input::BTN_A, 0x10},         {input::BTN_Y, 0x20},
                 {input::BTN_B, 0x40},          {input::BTN_ZL, 0x80},        {input::BTN_R, 0x200},
                 {input::BTN_PLUS, 0x400},      {input::BTN_HOME, 0x800},     {input::BTN_MINUS, 0x1000},
                 {input::BTN_L, 0x2000},        {input::BTN_DOWN, 0x4000},    {input::BTN_RIGHT, 0x8000},
                 {input::BTN_STICK_R, 0x10000}, {input::BTN_STICK_L, 0x20000}};
    uint32_t result = 0;
    for (auto pair : pairs)
        if (v & pair.vpad)
            result |= pair.pro;
    return result;
}
} // namespace uwp
