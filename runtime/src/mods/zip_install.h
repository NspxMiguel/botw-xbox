// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <string_view>
namespace mods {
struct InstallResult {
    std::filesystem::path path;
    bool graphics = false;
};
// Compare a release directory with either legacy BreathOfTheWild_* or modern
// src/BreathOfTheWild/<pack> identities. No untrusted path is joined to disk.
bool selected_pack_directory(const std::filesystem::path &, std::string_view selection);
// Shared root detection for ZIP and libarchive extraction.
std::filesystem::path find_root(const std::filesystem::path &, int depth, bool graphics);
// Transactional install; all untrusted paths are validated before extraction.
// Throws on malformed/unsupported archives and removes staging/partial output.
InstallResult install_zip(const std::filesystem::path &archive, const std::filesystem::path &mods_root,
                          std::string_view name, std::string_view pack_path = {});
} // namespace mods
