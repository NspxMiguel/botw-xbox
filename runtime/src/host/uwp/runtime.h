// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#if defined(_WIN32)
#include <winrt/Windows.Foundation.h>
#endif
namespace uwp {
#if defined(_WIN32)
winrt::Windows::Foundation::IAsyncAction configure_output_mode();
#endif
bool take_output_mode_change();
void refresh_output_mode(int &width, int &height);
float runtime_max_render_scale();
float runtime_render_scale();
void set_runtime_render_scale(float value);
bool prepare_runtime(const std::string &game_root, const std::string &user_root);
void start_runtime(void *window, int width, int height);
} // namespace uwp
