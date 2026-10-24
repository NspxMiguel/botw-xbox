// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <atomic>
#include <optional>

namespace uwp {
enum class GamepadScreen { Off, Corner, Swap };
inline std::atomic<GamepadScreen> gamepad_screen{GamepadScreen::Off};
inline GamepadScreen next_gamepad_screen(GamepadScreen mode, int direction = 1) {
    return static_cast<GamepadScreen>((static_cast<int>(mode) + (direction < 0 ? 2 : 1)) % 3);
}
inline void swap_gamepad_screen() {
    auto mode = gamepad_screen.load();
    gamepad_screen = mode == GamepadScreen::Swap ? GamepadScreen::Off : GamepadScreen::Swap;
}
struct ScanoutPlan {
    int primary = -1; // 0: TV, 1: DRC, -1: neither has supplied a frame
    bool corner = false;
};
inline ScanoutPlan scanout_plan(GamepadScreen mode, bool tv, bool drc) {
    return {mode == GamepadScreen::Swap && drc ? 1
            : tv                               ? 0
            : drc                              ? 1
                                               : -1,
            mode == GamepadScreen::Corner && tv && drc};
}
struct ScreenRect {
    int x, y, width, height;
};
inline ScreenRect gamepad_fit(int width, int height, int source_width, int source_height) {
    int w = std::max(1, width), h = std::max(1, height);
    if (source_width > 0 && source_height > 0) {
        h = std::max(1, w * source_height / source_width);
        if (h > height) {
            h = std::max(1, height);
            w = std::max(1, h * source_width / source_height);
        }
    }
    return {(width - w) / 2, (height - h) / 2, w, h};
}
inline ScreenRect gamepad_corner(int width, int height, int source_width, int source_height) {
    const int margin = std::max(1, std::min(width, height) / 40);
    const auto rect = gamepad_fit(width / 3, height / 3, source_width, source_height);
    // Keep BotW's hearts (top left) and minimap (bottom right) visible.
    return {width - margin - rect.width, margin, rect.width, rect.height};
}
// Backend-owned snapshots. A release invalidates the packet before its resource ID can be reused.
// This metadata contains no guest pointer or native descriptor. D3D12 owns the image snapshots.
template <class Packet> struct ScanoutFrames {
    std::optional<Packet> frames[2];
    void store(const Packet &packet) { frames[packet.padView ? 1 : 0] = packet; }
    template <class Id> void release(Id id) {
        for (auto &frame : frames)
            if (frame && (frame->view == id || frame->vertexShader == id || frame->pixelShader == id))
                frame.reset();
    }
};
} // namespace uwp
