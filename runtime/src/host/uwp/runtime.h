// SPDX-License-Identifier: MIT
#pragma once
#include <string>
namespace uwp {
bool prepare_runtime(const std::string &game_root, const std::string &user_root);
void start_runtime(void *window, int width, int height);
} // namespace uwp
