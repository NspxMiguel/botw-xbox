// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mods::local {
// Empty is the legacy drive-root layout. All roots are scanned, never just the first.
inline constexpr std::array<std::string_view, 3> roots{"botw-mods", "mods", ""};
inline bool equal_name(std::string_view a, std::string_view b) {
    auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; };
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i]))
            return false;
    return true;
}
inline bool component(std::string_view name) {
    if (name.empty() || name.front() == '.' || name.back() == '.' || name.back() == ' ')
        return false;
    for (unsigned char c : name)
        if (c < 32 || std::string_view("/\\:<>\"|?*").find(char(c)) != std::string_view::npos)
            return false;
    auto base = name.substr(0, name.find('.'));
    for (auto reserved :
         {"con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
          "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"})
        if (equal_name(base, reserved))
            return false;
    return true;
}
inline bool content_folder(std::string_view name) {
    return equal_name(name, "content") || equal_name(name, "aoc");
}
// Source metadata must never become launcher state or a graphics lock on import.
inline bool launcher_file(std::string_view name) {
    return equal_name(name, "enabled.txt") || equal_name(name, "enabled.txt.tmp") ||
           equal_name(name, ".store-receipt") || equal_name(name, ".imported-graphics");
}
inline std::string import_name(std::string_view name, bool root, bool folder) {
    if (!component(name))
        throw std::runtime_error("Invalid mod filename");
    if (root && folder && content_folder(name))
        return equal_name(name, "content") ? "content" : "aoc";
    if (root && !folder && equal_name(name, "rules.txt"))
        return "rules.txt";
    return std::string(name);
}
} // namespace mods::local
