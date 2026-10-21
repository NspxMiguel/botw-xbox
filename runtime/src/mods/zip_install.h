// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <string_view>
namespace mods {
struct InstallResult {
    std::filesystem::path path;
    bool graphics = false;
};
// Transactional install; all untrusted paths are validated before extraction.
// Throws on malformed/unsupported archives and removes staging/partial output.
InstallResult install_zip(const std::filesystem::path &archive, const std::filesystem::path &mods_root,
                          std::string_view name);
} // namespace mods
