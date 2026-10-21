// SPDX-License-Identifier: MIT
#pragma once
#include <string>
namespace uwp {
float runtime_max_render_scale();
float runtime_render_scale();
void set_runtime_render_scale(float value);
bool prepare_runtime(const std::string &game_root, const std::string &user_root);
void start_runtime(void *window, int width, int height);
} // namespace uwp
