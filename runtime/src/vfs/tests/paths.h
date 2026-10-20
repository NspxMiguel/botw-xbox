// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <string>

inline std::filesystem::path utf8_path(const std::string &text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()));
}
inline std::string utf8_string(const std::filesystem::path &path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}
