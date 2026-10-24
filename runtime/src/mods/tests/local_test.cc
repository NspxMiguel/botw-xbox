// SPDX-License-Identifier: MIT
#include "../../vfs/overlay.h"
#include "../catalog.h"
#include "../local.h"
#include "../store.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>

namespace fs = std::filesystem;
void file(fs::path path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "synthetic";
}
int main() {
    using namespace mods;
    using namespace mods::store;
    for (auto name : {"PT-BR", "Tradução", "# Translation", "Voice (Xbox)", "FPS++", "sample.zip"})
        assert(local::component(name));
    for (auto name : {"",         ".",      "..",     ".usb-import", "../escape", "a/b",     "a\\b", "E:mod",
                      "a:stream", "a\nmod", "a\rmod", "mod ",        "mod.",      "a*",      "a?",   "a|",
                      "a<",       "a>",     "a\"",    "CON",         "nul.txt",   "Lpt9.zip"})
        assert(!local::component(name));
    assert(!local::component(std::string("a\0b", 3)));
    assert(local::content_folder("CONTENT") && local::content_folder("AoC"));
    assert(!local::content_folder("content-other"));
    assert(local::launcher_file("ENABLED.TXT") && local::launcher_file(".STORE-RECEIPT"));
    assert(local::launcher_file(".imported-graphics") && !local::launcher_file("rules.txt"));
    assert(local::roots[0] == "botw-mods");
    assert(local::import_name("CONTENT", true, true) == "content");
    assert(local::import_name("AoC", true, true) == "aoc");
    assert(local::import_name("RULES.TXT", true, false) == "rules.txt");
    assert(local::import_name("CONTENT", false, true) == "CONTENT");
    assert(local::import_name("RULES.TXT", false, false) == "RULES.TXT");
    assert(local::import_name("Tradução", true, true) == "Tradução");
    try {
        local::import_name("../escape", false, false);
        assert(false);
    } catch (std::runtime_error const &) {
    }

    auto root =
        fs::temp_directory_path() /
        ("usb-mod-paths-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        fs::path root;
        ~Cleanup() { fs::remove_all(root); }
    } cleanup{root};
    auto drive = root / "usb";
    file(drive / "botw-mods/Tradução/CONTENT/Message/test.msbt");
    file(drive / "botw-mods/Voice/aoc/Voice/test.bfstm");
    file(drive / "botw-mods/Graphics/RULES.TXT");
    file(drive / "botw-mods/empty/unrelated.txt");
    file(drive / "botw-mods/.usb-import/content/test.pack");
    file(drive / "botw-mods/bad\nname/content/test.pack");
    file(drive / "mods/Legacy/content/test.pack");
    file(drive / "RootPack/content/test.pack");
    file(drive / "botw-mods/Archive.ZIP");
    fs::create_directory_symlink(drive / "RootPack", drive / "botw-mods/LinkedPack");
    fs::create_directory(drive / "botw-mods/LinkedContent");
    fs::create_directory_symlink(drive / "RootPack/content", drive / "botw-mods/LinkedContent/content");
    fs::create_directory(drive / "botw-mods/LinkedRules");
    fs::create_symlink(drive / "botw-mods/Graphics/RULES.TXT", drive / "botw-mods/LinkedRules/rules.txt");
    auto source = make_source(SourceId::local, {}, {}, {drive, drive});
    auto page = source->search("", Category::all, 1);
    std::set<std::string> titles;
    for (auto &card : page.cards) {
        assert(card.source == "USB");
        assert(titles.insert(card.title).second);
        if (card.title == "Graphics")
            assert(card.category == Category::graphics);
        if (card.title == "Archive.ZIP")
            assert(card.downloads.size() == 1 && card.downloads[0].url == card.id);
    }
    assert((titles ==
            std::set<std::string>{"Tradução", "Voice", "Graphics", "Legacy", "RootPack", "Archive.ZIP"}));
    assert(source->search("TRADUCAO", Category::all, 1).cards.size() == 1);
    assert(source->search("", Category::graphics, 1).cards.size() == 1);
    assert(source->search("", Category::all, 2).cards.empty());
    fs::rename(drive, root / "disconnected");
    assert(source->search("", Category::all, 1).cards.empty());
    file(root / "second-drive/botw-mods/Second/content/test.pack");
    auto multiple = make_source(SourceId::local, {}, {}, {drive, root / "second-drive"});
    assert(multiple->search("", Category::all, 1).cards.size() == 1);
    fs::rename(root / "disconnected", drive);
    assert(source->search("", Category::all, 1).cards.size() == 6);

    // A broker copies to a hidden stage. Even a complete wrapper is invisible until publication.
    auto installed = root / "local/mods";
    file(installed / ".usb-import/content/test.pack");
    ModCatalog catalog;
    catalog.load(installed, {});
    assert(catalog.list().empty());
    fs::rename(installed / ".usb-import", installed / "Imported");
    finish_install(installed / "Imported", [&] {
        catalog.load(installed, {});
        assert(catalog.setEnabled("file:Imported", true));
        catalog.save();
    });
    fs::remove_all(drive);
    catalog.load(installed, {});
    assert(catalog.list().size() == 1 && catalog.list()[0].enabled);
    auto game_file = root / "local/game/content/test.pack";
    file(game_file);
    vfs::Overlay overlay;
    overlay.configure_defaults(root / "local/game", root / "local", installed);
    assert(fs::path(overlay.resolve(game_file.string())).lexically_normal() ==
           installed / "Imported/content/test.pack");
    // A failed publication cannot enable a partial mod or disturb the existing selection.
    file(installed / "Failed/content/test.pack");
    try {
        finish_install(installed / "Failed", [] { throw std::runtime_error("Drive removed"); });
        assert(false);
    } catch (std::runtime_error const &) {
    }
    assert(!fs::exists(installed / "Failed"));
    catalog.load(installed, {});
    assert(catalog.list().size() == 1 && catalog.list()[0].enabled);
    file(installed / "Graphics/rules.txt");
    file(installed / "Graphics/.imported-graphics");
    catalog.load(installed, {});
    assert(catalog.list().size() == 2);
    assert(!catalog.setEnabled("file:Graphics", true));
    assert(catalog.setEnabled("file:Imported", false));
    catalog.save();
    catalog.load(installed, {});
    for (auto &entry : catalog.list())
        assert(!entry.enabled);
    std::cout << "USB paths, discovery, removal and staged publication passed\n";
}
