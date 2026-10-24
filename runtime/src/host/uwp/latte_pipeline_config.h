// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace uwp {
inline const char *ReadLattePipelineMode(const std::filesystem::path &file) {
    std::ifstream input(file);
    if (!input)
        return "2";
    const std::string mode((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto first = mode.find_first_not_of(" \t\r\n");
    const auto last = mode.find_last_not_of(" \t\r\n");
    const auto selected = first == std::string::npos ? std::string{} : mode.substr(first, last - first + 1);
    if (selected == "0")
        return "0";
    if (selected == "2")
        return "2";
    if (selected == "3")
        return "3";
    return "1";
}
} // namespace uwp
