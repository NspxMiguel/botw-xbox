// SPDX-License-Identifier: MIT
#pragma once
#include "zip_install.h"
namespace mods {
// ZIP uses the small audited installer. 7z/RAR use linked, packaged libarchive;
// no downloaded executable, shell process or dynamic library is ever loaded.
InstallResult install_archive(const std::filesystem::path &archive, const std::filesystem::path &mods_root,
                              std::string_view name, std::string_view pack_path = {});
bool supported_archive(std::string_view name);
} // namespace mods
