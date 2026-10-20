// SPDX-License-Identifier: MIT
#pragma once
#include "../packs/packs.h"
#include <filesystem>
#include <string>
#include <vector>
namespace mods {
enum class Kind { translation, dubbing, graphics, files };
struct ModInfo {
    std::string id, name;
    Kind kind = Kind::files;
    bool enabled = false, locked = false;
    std::string note;
};
class ModCatalog {
    std::filesystem::path root_;
    std::vector<ModInfo> entries_;
    std::vector<std::string> unknown_;  // enabled.txt lines the catalog cannot list; save() keeps them

  public:
    void load(const std::filesystem::path &mods_root, const packs::BuildPlan &plan);
    const std::vector<ModInfo> &list() const { return entries_; }
    bool setEnabled(const std::string &id, bool enabled);
    bool move(const std::string &id, size_t index);
    // Throws filesystem_error/ios_base::failure on failure; old selection
    // survives.
    void save() const;
};
packs::BuildPlan filtered_plan(const packs::BuildPlan &, const ModCatalog &);
} // namespace mods
