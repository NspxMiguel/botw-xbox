// SPDX-License-Identifier: MIT
#pragma once
#include <d2d1.h>
namespace uwp {
namespace art_style {
inline constexpr unsigned store_silver = 0xf2f4f5, store_ink = 0x18191b, store_muted = 0x53565b;
inline constexpr float store_title_size = 48, store_card_size = 24, store_body_size = 20,
                       store_meta_size = 17;
inline constexpr float width = 1280, height = 720, button_h = 56;
inline constexpr float margin = 64;
inline constexpr float title_size = 34, body_size = 17;
inline D2D1_COLOR_F black(float alpha = 1) { return D2D1::ColorF(0, 0, 0, alpha); }
inline D2D1_COLOR_F white(float alpha = 1) { return D2D1::ColorF(1, 1, 1, alpha); }
inline D2D1_COLOR_F warning() { return D2D1::ColorF(.72f, .43f, .05f); }
inline D2D1_COLOR_F missing() { return D2D1::ColorF(.72f, .08f, .08f); }
inline D2D1_COLOR_F optional() { return D2D1::ColorF(.4f, .4f, .4f); }
inline constexpr float quick_dim = .65f, quick_surface = .10f;
} // namespace art_style
} // namespace uwp
