// SPDX-License-Identifier: MIT
#include "../gamepad_screen.h"
#include "../quick_menu.h"
#include "../strings.h"
#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    using namespace uwp;
    assert(gamepad_screen.load() == GamepadScreen::Off);
    assert(next_gamepad_screen(GamepadScreen::Off) == GamepadScreen::Corner);
    assert(next_gamepad_screen(GamepadScreen::Corner) == GamepadScreen::Swap);
    assert(next_gamepad_screen(GamepadScreen::Swap) == GamepadScreen::Off);
    assert(next_gamepad_screen(GamepadScreen::Off, -1) == GamepadScreen::Swap);
    for (auto mode : {GamepadScreen::Off, GamepadScreen::Corner, GamepadScreen::Swap}) {
        assert(scanout_plan(mode, false, false).primary == -1);
        assert(scanout_plan(mode, true, false).primary == 0);
        assert(scanout_plan(mode, false, true).primary == 1);
        auto plan = scanout_plan(mode, true, true);
        assert(plan.primary == (mode == GamepadScreen::Swap ? 1 : 0));
        assert(plan.corner == (mode == GamepadScreen::Corner));
    }
    gamepad_screen = GamepadScreen::Corner;
    swap_gamepad_screen();
    assert(gamepad_screen == GamepadScreen::Swap);
    swap_gamepad_screen();
    assert(gamepad_screen == GamepadScreen::Off);
    struct Packet {
        int view, vertexShader = 8, pixelShader = 9;
        bool padView;
    };
    for (bool drc_first : {false, true}) {
        ScanoutFrames<Packet> frames;
        frames.store({1, 8, 9, drc_first});
        frames.store({2, 8, 9, !drc_first});
        assert(frames.frames[0] && frames.frames[1]);
        frames.release(1);
        assert(!frames.frames[drc_first ? 1 : 0] && frames.frames[drc_first ? 0 : 1]);
        frames.store({0, 8, 9, drc_first}); // Captured image no longer depends on the guest view.
        frames.release(1);
        assert(frames.frames[drc_first ? 1 : 0]);
        frames.store({3, 8, 9, drc_first});
        frames.release(8); // Shader lifetime is checked as well as view lifetime.
        assert(!frames.frames[0] && !frames.frames[1]);
    }
    for (auto size : {std::pair{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}, {720, 1280}}) {
        auto r = gamepad_corner(size.first, size.second, 854, 480);
        assert(r.width > 0 && r.height > 0 && r.x > 0 && r.y > 0);
        assert(r.x + r.width < size.first && r.y + r.height < size.second);
        assert(r.width <= size.first / 3 && r.height <= size.second / 3);
        assert(std::abs(double(r.width) / r.height - 854. / 480) < .02);
        assert(r.y == std::max(1, std::min(size.first, size.second) / 40));
        auto full = gamepad_fit(size.first, size.second, 854, 480);
        assert(full.width <= size.first && full.height <= size.second);
        assert(full.x == (size.first - full.width) / 2 && full.y == (size.second - full.height) / 2);
    }
    HoldChord chord;
    auto update = [&](uint32_t buttons, double now) { return chord.update(buttons, now, true); };
    update(view, 0);
    update(view, .49);
    assert(!chord.toggle_screen && !chord.guest_buttons);
    update(view, .51);
    assert(chord.toggle_screen && !chord.guest_buttons);
    update(view, 1);
    assert(!chord.toggle_screen && !chord.guest_buttons);
    update(0, 1.1);
    update(view, 2);
    update(0, 2.1);
    assert(chord.guest_buttons == view && !chord.toggle_screen); // Short View still opens the map.
    update(0, 2.2);
    update(view | rs, 3);
    assert(chord.toggle_tilt && !chord.guest_buttons);
    update(rs, 3.1); // Partial release must not open the telescope.
    assert(!chord.guest_buttons && !chord.toggle_tilt);
    update(0, 3.2);
    update(view | x, 4);
    assert(chord.touch && !chord.guest_buttons);
    update(view, 4.1);
    assert(!chord.touch && !chord.guest_buttons);
    update(0, 4.2);
    update(view | y, 5);
    assert(chord.toggle_screen && !chord.guest_buttons);
    update(0, 5.1);
    update(view | menu | rs | x, 6);
    assert(!chord.toggle_tilt && !chord.touch);
    assert(update(view | menu, 6.51)); // Quick menu takes precedence over View hold.
    assert(!chord.toggle_screen && !chord.guest_buttons);
    chord.reset();
    update(view, 10);
    update(view, 11);
    assert(!chord.toggle_screen && !chord.guest_buttons);
    update(view | rs, 11.1);
    assert(!chord.toggle_tilt && !chord.guest_buttons);
    update(view | x, 11.15);
    assert(!chord.touch && !chord.guest_buttons);
    update(0, 11.2);
    update(view, 12);
    update(view, 12.51);
    assert(chord.toggle_screen); // Release rearms hold-View after focus regain.
    Controller controller;
    Reading reading{b | ls, 1, 0, .7, .8, 0, 1};
    auto mapped = controller.update(reading);
    auto normal = mapped.pad;
    apply_motion_controls(mapped.pad, reading, false);
    assert(mapped.pad.buttons == normal.buttons && mapped.pad.rx == normal.rx);
    apply_motion_controls(mapped.pad, reading, true);
    assert(mapped.pad.virtual_motion && mapped.pad.recenter_motion);
    assert(mapped.pad.tilt_x == normal.ry && mapped.pad.tilt_y == normal.rx && mapped.pad.tilt_z == 1);
    assert(mapped.pad.rx == 0 && mapped.pad.ry == 0 && mapped.pad.lx == 1);
    assert(mapped.pad.buttons == input::BTN_A); // Cancel/confirm retained; camera/triggers/recenter consumed.
    for (auto key : {Text::gamepad_screen, Text::gamepad_off, Text::gamepad_corner, Text::gamepad_swap,
                     Text::motion_hint}) {
        assert(!text(key, false).empty() && !text(key, true).empty());
    }
    std::cout << "PASS: scanout routing/lifetimes/aspect, View arbitration, touch and apparatus controls\n";
}
