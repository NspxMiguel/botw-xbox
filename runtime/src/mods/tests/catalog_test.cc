// SPDX-License-Identifier: MIT
#include "../../vfs/overlay.h"
#include "../catalog.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <source_location>
#include <stdexcept>
namespace fs = std::filesystem;
void require(bool value, const std::source_location location = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("Mod catalog assertion failed at " + std::to_string(location.line()));
}
void file(const fs::path &p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "synthetic";
}
int main() {
    auto root = fs::temp_directory_path() /
                ("botw-mods-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        fs::path root;
        ~Cleanup() { fs::remove_all(root); }
    } cleanup{root};
    auto mods_root = root / "mods";
    file(mods_root / "Translation/content/Message/test.msbt");
    file(mods_root / "Mals/content/Mals/dummy");
    file(mods_root / "Voice/aoc/Voice/dummy");
    file(mods_root / "Stream/content/Sound/Resource/Stream/dummy");
    file(mods_root / "Files/content/shared");
    file(mods_root / "Translation/content/shared");
    file(mods_root / "Ignore/rules.txt");
    file(mods_root / "# name /content/dummy");
    packs::BuildPlan plan;
    plan.fingerprint = "original";
    packs::Pack first;
    first.name = "Shader pack";
    first.shaders.push_back({});
    first.textures.push_back({});
    plan.packs = {first, first};
    mods::ModCatalog c;
    c.load(mods_root, plan);
    require(c.list().size() == 8);
    for (const auto &e : c.list()) {
        if (e.name == "Translation" || e.name == "Mals")
            require(e.kind == mods::Kind::translation);
        if (e.name == "Voice" || e.name == "Stream")
            require(e.kind == mods::Kind::dubbing);
        if (e.name == "Files")
            require(e.kind == mods::Kind::files && !e.enabled);
    }
    auto pack_id = c.list()[6].id;
    require(c.setEnabled(pack_id, false));
    auto filtered = mods::filtered_plan(plan, c);
    require(filtered.packs[0].shaders.empty() && filtered.packs[0].textures.empty());
    require(filtered.packs[1].shaders.size() == 1 && filtered.fingerprint != plan.fingerprint);
    require(c.move(c.list()[7].id, 6));
    auto reordered = mods::filtered_plan(plan, c);
    require(reordered.packs[0].shaders.size() == 1 && reordered.packs[1].shaders.empty());
    require(c.setEnabled("file:Files", true));
    require(c.setEnabled("file:Translation", true));
    require(c.setEnabled("file:# name ", true));
    require(c.move("file:Translation", 0));
    require(c.move("file:Files", c.list().size() - 1));
    require(!c.move("missing", 0) && !c.setEnabled("missing", false));
    c.save();
    c.save();
    mods::ModCatalog restored;
    restored.load(mods_root, plan);
    require(restored.list().size() == c.list().size());
    for (size_t i = 0; i < c.list().size(); ++i) {
        require(c.list()[i].id == restored.list()[i].id);
        require(c.list()[i].enabled == restored.list()[i].enabled);
    }
    file(root / "game/content/shared");
    vfs::Overlay overlay;
    overlay.configure_defaults(root / "game", root, mods_root);
    require(fs::path(overlay.resolve((root / "game/content/shared").string())).lexically_normal() ==
            (mods_root / "Files/content/shared").string());
    require(fs::path(overlay.resolve((root / "game/content/dummy").string())).lexically_normal() ==
            (mods_root / "# name /content/dummy").string());
    plan.initial_memory.push_back({1, {2}});
    restored.load(mods_root, plan);
    for (const auto &e : restored.list())
        if (e.kind == mods::Kind::graphics) {
            require(e.enabled && e.locked && e.note == "rebuild to change");
            require(!restored.setEnabled(e.id, false));
        }
    require(mods::filtered_plan(plan, restored).initial_memory.size() == 1);
    // Legacy path-only lists remain authoritative; unrelated folders stay
    // disabled.
    std::ofstream(mods_root / "enabled.txt") << "Translation\nFiles\n";
    restored.load(mods_root, {});
    require(restored.list()[0].id == "file:Translation" && restored.list()[1].id == "file:Files");
    std::ofstream(mods_root / "enabled.txt") << "Files\nTranslation\nFiles\n";
    restored.load(mods_root, {});
    require(restored.list()[0].id == "file:Translation" && restored.list()[1].id == "file:Files");
    restored.setEnabled("file:Files", false);
    restored.save();
    vfs::Overlay disabled;
    disabled.configure_defaults(root / "game", root, mods_root);
    require(fs::path(disabled.resolve((root / "game/content/shared").string())).lexically_normal() ==
            mods_root / "Translation/content/shared");
    // A plain tree the catalog cannot list must survive a save from the Mods screen.
    std::ofstream(mods_root / "enabled.txt") << "PlainMod\nTranslation\n";
    restored.load(mods_root, {});
    restored.save();
    std::ifstream kept(mods_root / "enabled.txt");
    std::string text((std::istreambuf_iterator<char>(kept)), {});
    require(text.find("\nPlainMod\n") != text.npos);
    std::cout << "kind detection, toggle, move, round-trip, overlay order, unknown lines kept, "
                 "locked packs, shader filtering: PASS\n";
}
