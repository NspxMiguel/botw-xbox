// SPDX-License-Identifier: MIT
#pragma once
#include "controller.h"
#include "setup.h"
#include <atomic>
#include <mutex>
#include <thread>
namespace uwp {
struct HoldChord {
    double since = -1, pending_since = -1, replay_until = -1;
    bool latched = false, passthrough = false;
    uint32_t pending = 0, guest_buttons = 0, replay = 0;
    bool update(uint32_t buttons, double now) {
        constexpr auto mask = view | menu;
        auto held = buttons & mask;
        guest_buttons = (buttons & ~mask) | (now < replay_until ? replay : 0);
        if (latched) {
            since = -1;
            if (!held)
                latched = false;
            return false;
        }
        if (passthrough) {
            guest_buttons |= held;
            if (!held)
                passthrough = false;
            return false;
        }
        if (held && pending_since < 0)
            pending_since = now;
        pending |= held;
        if (held == mask) {
            if (since < 0)
                since = now;
            if (now - since >= .5) {
                latched = true;
                pending = 0;
                pending_since = -1;
                return true;
            }
        } else {
            since = -1;
            // A released tap lasts long enough for a 30 Hz guest sample.
            // A lone long hold passes through after 0.5 s.
            if (pending && (!held || now - pending_since >= .5)) {
                replay = pending & ~held;
                replay_until = now + .05;
                guest_buttons |= pending;
                pending = 0;
                pending_since = -1;
                passthrough = held != 0;
            }
        }
        return false;
    }
};
inline float cycle_scale(float value, float maximum) {
    for (float next : {1.f, 1.5f, 2.f, 3.f})
        if (next > value && next <= maximum)
            return next;
    return 1.f;
}
// UI owns input/preferences. GPU owns the snapshot and overlay resources.
struct QuickMenu {
    std::atomic<bool> requested{false}, paused{false}, capture{false};
    std::atomic<int> capture_result{0};
    std::mutex mutex;
    int focus = 0;
    bool reduced_motion = false;
    std::vector<std::wstring> rows;
    std::wstring title, hints, screenshot_path;
};
inline QuickMenu quick_menu;
inline void wait_for_guest_resume() {
    while (quick_menu.paused.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
}
} // namespace uwp
