// SPDX-License-Identifier: MIT
#include "../overlay.h"

#include "paths.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
static std::vector<std::string> logs;
static void log_line(const std::string &line) { logs.push_back(line); }
static void put(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}
static std::string read(const std::string &path) {
    std::ifstream in(utf8_path(path));
    return {std::istreambuf_iterator<char>(in), {}};
}
static uintmax_t size(const std::string &path) { return fs::file_size(utf8_path(path)); }
int main() {
    const auto root = fs::temp_directory_path() /
                      ("botw-overlay-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto game = root / utf8_path("jogo ação"), low = root / utf8_path("mods/tradução"),
               high = root / utf8_path("mods/dublagem português");
    put(game / "content/Pack/Message.sarc", "base");
    put(game / "content/Pack/BaseOnly", "base only");
    put(game / "content/Pack/Tree/Hidden", "hidden");
    put(game / "aoc/0010/Voice.bars", "base voice");
    put(game / "code/Main", "code");
    put(low / "rules.txt", "[Definition]\nname = Translation\nversion = 6\n");
    put(low / "content/pack/message.SARC", "low");
    put(low / "content/pack/Added", "new");
    put(low / "content/Pack/Tree/LowChild", "child");
    put(high / "CONTENT/PACK/MESSAGE.sarc", "Portuguese messages");
    put(high / "aoc/0010/VOICE.BARS", "Portuguese voices");
    put(high / "content/pack/Tree", "file masks directory");
    vfs::Overlay overlay;
    put(root / "graphics/rules.txt", "[Definition]\nname = Graphics\nversion = 6\n");
    overlay.configure(game, {{"graphics only", root / "graphics"}});
    assert(!overlay.active());
    overlay.configure(game, {});
    assert(!overlay.active());
    assert(overlay.resolve("unchanged") == "unchanged");
    std::vector<vfs::Entry> entries;
    bool exists = false;
    assert(!overlay.list(utf8_string(game / "content/Pack"), entries, exists));
    const auto message = utf8_string(game / "content/PACK/message.sarc");
    overlay.configure(game, {{"low", low}, {"disabled", high, false}}, log_line);
    assert(read(overlay.resolve(message)) == "low");
    logs.clear();
    overlay.configure(game, {{"low", low}, {"high", high}}, log_line);
    assert(read(overlay.resolve(message)) == "Portuguese messages");
    assert(size(overlay.resolve(message)) == 19);
    assert(logs.size() == 1);
    assert(read(overlay.resolve(utf8_string(game / "AOC/0010/voice.bars"))) == "Portuguese voices");
    assert(logs.size() == 2);
    assert(read(overlay.resolve(utf8_string(game / "content/pack/baseonly"))) == "base only");
    assert(overlay.list(utf8_string(game / "content/pAcK"), entries, exists) && exists);
    assert(entries.size() == 4);
    size_t messages = 0;
    for (const auto &entry : entries) {
        if (entry.name == "MESSAGE.sarc") {
            ++messages;
            assert(size(entry.host) == 19);
        }
    }
    assert(messages == 1 && logs.size() == 4); // Added and Tree first encountered in listing.
    assert(!fs::exists(utf8_path(overlay.resolve(utf8_string(game / "content/Pack/Tree/Hidden")))));
    assert(!fs::exists(utf8_path(overlay.resolve(utf8_string(game / "content/Pack/Tree/LowChild")))));
    assert(!fs::exists(utf8_path(overlay.resolve(utf8_string(game / "content/missing")))));
    assert(overlay.list(utf8_string(game / "content/missing"), entries, exists) && !exists);
    assert(overlay.resolve(utf8_string(game / "code/Main")) == utf8_string(game / "code/Main"));
    assert(overlay.resolve(utf8_string(game / "content/../../secret")) == utf8_string(game / "content/../../secret"));
    put(high / "content/NewDir/Only", "mod-only directory");
    overlay.configure(game, {{"high", high}, {"low", low}});
    assert(read(overlay.resolve(message)) == "low");
    assert(overlay.list(utf8_string(game / "content/newdir"), entries, exists) && exists && entries.size() == 1);
    assert(read(entries.front().host) == "mod-only directory");
    // A later directory replaces an earlier file, and exposes base children.
    assert(overlay.list(utf8_string(game / "content/pack/tree"), entries, exists) && exists && entries.size() == 2);
    overlay.configure(game, {{"missing", root / "does-not-exist"}, {"plain", low / "content"}});
    assert(read(overlay.resolve(message)) == "low");
    assert(overlay.list(utf8_string(game / "content/pack/"), entries, exists) && exists && entries.size() == 4);
    assert(!fs::exists(utf8_path(overlay.resolve(utf8_string(game / "content/rules.txt")))));
    overlay.configure(game, {{"plain", low / "content", true, "content"}});
    assert(read(overlay.resolve(message)) == "low");
    put(root / "mods/enabled.txt", "# order\ntradução\ndublagem português\n");
    put(root / "local/mods/enabled.txt", "../../mods/tradução\n");
    vfs::Overlay defaults;
    defaults.configure_defaults(utf8_path(utf8_string(game) + "/"), root / "local");
    assert(read(defaults.resolve(message)) == "low");
    // Explicit launcher configuration must survive FSInit's fallback.
    defaults.configure(game, {{"low", low}});
    defaults.configure_defaults(game, root / "local");
    assert(read(defaults.resolve(message)) == "low");
    vfs::Overlay chosen;
    put(root / "selected/enabled.txt", (utf8_string(low) + "\n"));
    chosen.configure_defaults(game, root / "local", root / "selected");
    assert(read(chosen.resolve(message)) == "low");
    fs::remove_all(root);
    std::cout << "overlay tests passed\n";
}
