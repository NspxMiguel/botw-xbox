// SPDX-License-Identifier: MIT
#include "../controller.h"
#include "../launcher.h"
#include "../strings.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
int checks = 0;
void check(bool condition) {
    ++checks;
    if (!condition) {
        std::cerr << "Failed check " << checks << '\n';
        std::exit(1);
    }
}
void launcher() {
    using namespace uwp;
    Candidate local{"local", Source::local, true, true, true};
    Candidate usb{"usb", Source::removable, true, true, true};
    Candidate remembered{"picked", Source::remembered, true, true, true};
    for (bool setup : {false, true})
        for (bool automatic : {false, true}) {
            Preferences p{setup, automatic};
            auto empty = decide({}, p);
            check(empty.select_game && !empty.ask_auto_launch && !empty.launch);
            auto found = decide({usb, remembered, local}, p);
            check(found.game && found.game->path == "local" && !found.select_game);
            check(found.ask_auto_launch == !setup);
            check(found.launch == (setup && automatic));
            check(!decide({local}, p, true).launch);
            check(decide({usb, remembered}, p).game->path == "picked");
        }
    // Independently missing RPX/content/meta/path must never launch, even with auto enabled.
    for (unsigned mask = 0; mask < 16; ++mask) {
        Candidate c{mask & 1 ? "game" : "", Source::local, bool(mask & 2), bool(mask & 4), bool(mask & 8)};
        auto d = decide({c}, Preferences{true, true});
        check(d.launch == (mask == 15));
        check(d.select_game == (mask != 15));
    }
    local.rpx = false;
    check(decide({local, usb}, {}).game->path == "usb");
    check(decide({local}, {true, true}).select_game); // Unplugged or invalid remembered folder.
}
void input_mapping() {
    using namespace uwp;
    Controller c;
    const struct {
        uint32_t button, guest;
    } cases[] = {{a, input::BTN_B},        {b, input::BTN_A},         {x, input::BTN_Y},
                 {y, input::BTN_X},        {up, input::BTN_UP},       {down, input::BTN_DOWN},
                 {left, input::BTN_LEFT},  {right, input::BTN_RIGHT}, {menu, input::BTN_PLUS},
                 {view, input::BTN_MINUS}, {lb, input::BTN_L},        {rb, input::BTN_R},
                 {ls, input::BTN_STICK_L}, {rs, input::BTN_STICK_R}};
    for (auto pair : cases)
        check(c.update({pair.button}).pad.buttons == pair.guest);
    check(c.update({a}, 1).pad.buttons == input::BTN_A);
    check(c.update({b}, 1).pad.buttons == input::BTN_B);
    check(c.update({x}, 1).pad.buttons == input::BTN_X);
    check(c.update({y}, 1).pad.buttons == input::BTN_Y);
    auto triggers = c.update({0, 0, 0, 0, 0, .5, .5});
    check(triggers.pad.buttons == 0);
    check(c.update({0, 0, 0, 0, 0, .51, 1}).pad.buttons == (input::BTN_ZL | input::BTN_ZR));
    auto home = c.update({view | menu, 1, 1, 1, 1, 1, 1});
    check(home.return_to_menu && home.pad.buttons == 0 && home.pad.lx == 0);
    check(!c.update({view | menu}).return_to_menu); // Held chord opens once.
    c.update({});
    check(c.update({menu | view}).return_to_menu);
    c.update({});
    auto screen = c.update({view | y});
    check(screen.toggle_gamepad_screen && screen.pad.buttons == 0);
    check(!c.update({view | y}).toggle_gamepad_screen);
    check(c.update({view}).pad.buttons == 0);           // Partial shortcut release cannot leak Minus.
    check(!c.update({view | y}).toggle_gamepad_screen); // Release the full chord to rearm.
    c.update({});
    auto both = c.update({view | menu | y});
    check(both.return_to_menu && !both.toggle_gamepad_screen);
    c.reset();
    check(!c.update({view | menu}).return_to_menu);
    check(c.update({a}).pad.buttons == 0); // Focus regain cannot leak a held accept button.
    c.update({});
    check(c.update({a}).pad.buttons == input::BTN_B);
    check(c.update({0, .1, .1}).pad.lx == 0);
    check(c.update({0, 1, 0, 0, -1}).pad.lx == 1);
    check(c.update({0, 1, 0, 0, -1}).pad.ry == -1);
    check(c.update({0, std::numeric_limits<double>::quiet_NaN(), 1}).pad.ly == 0);
    // Sweep the stick square: radial normalization never exceeds a unit vector or flips an axis.
    for (int x = -100; x <= 100; ++x)
        for (int y = -100; y <= 100; ++y) {
            auto s = c.update({0, x / 100., y / 100.}).pad;
            check(std::hypot(s.lx, s.ly) <= 1.000001f);
            check(s.lx * x >= 0 && s.ly * y >= 0);
        }
    check(pro_buttons(input::BTN_A | input::BTN_ZL | input::BTN_STICK_L) == 0x20090);
    check(pro_buttons(input::BTN_UP | input::BTN_DOWN | input::BTN_LEFT | input::BTN_RIGHT) == 0xC003);
    check(pro_buttons(input::BTN_TV | input::BTN_SYNC) == 0);
}
} // namespace
int main() {
    launcher();
    input_mapping();
    for (size_t i = 0; i < static_cast<size_t>(uwp::Text::count); ++i) {
        check(!uwp::text(static_cast<uwp::Text>(i), false).empty());
        check(!uwp::text(static_cast<uwp::Text>(i), true).empty());
    }
    std::cout << checks << " host checks passed\n";
}
