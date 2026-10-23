// SPDX-License-Identifier: MIT
#include "../diagnostics.h"
#include "../quick_menu.h"
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
using namespace uwp::setup;
namespace fs = std::filesystem;
void write(const fs::path &root, std::string name, std::string data = "x") {
    fs::create_directories((root / name).parent_path());
    std::ofstream(root / name, std::ios::binary).write(data.data(), std::streamsize(data.size()));
}
void exact(const Diagnosis &d, std::vector<std::pair<std::string, IssueKind>> expected) {
    assert(d.issues.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        assert(d.issues[i].path == expected[i].first);
        assert(d.issues[i].kind == expected[i].second);
    }
}
std::string zip_fixture(const std::string &name, bool damaged = false) {
    std::string image(30 + name.size(), '\0');
    auto put = [&](size_t at, uint64_t value, size_t count) {
        for (size_t i = 0; i < count; ++i)
            image[at + i] = char(value >> (8 * i));
    };
    put(0, damaged ? 0 : 0x04034b50, 4);
    put(26, name.size(), 2);
    image.replace(30, name.size(), name);
    auto directory = image.size();
    image.resize(directory + 46 + name.size());
    put(directory, 0x02014b50, 4);
    put(directory + 28, name.size(), 2);
    image.replace(directory + 46, name.size(), name);
    auto end = image.size();
    image.resize(end + 22);
    put(end, 0x06054b50, 4);
    put(end + 8, 1, 2);
    put(end + 10, 1, 2);
    put(end + 12, end - directory, 4);
    put(end + 16, directory, 4);
    return image;
}
std::string seven_fixture(const std::string &name) {
    std::string header{char(1), char(5), char(1), char(0x11), char(1 + (name.size() + 1) * 2), char(0)};
    for (auto c : name) {
        header += c;
        header += char(0);
    }
    header.append(4, '\0');
    std::string image(32, '\0');
    image.replace(0, 6, "\x37\x7a\xbc\xaf\x27\x1c");
    image[20] = char(header.size());
    image += header;
    return image;
}
void recognition_tests(const fs::path &root) {
    fs::create_directories(root);
    auto p = filesystem_probe(root);
    write(root, "photos.zip", zip_fixture("holiday/photo.jpg"));
    write(root, "game.zip", zip_fixture("dump/code/U-King.rpx", true));
    write(root, "wrapped.zip", zip_fixture("dump/game.wua"));
    write(root, "wrapped.7z", seven_fixture("dump/game.WUA"));
    write(root, "photos.7z", seven_fixture("holiday/photo.jpg"));
    write(root, "game.7z", seven_fixture("dump/content/Bootup.pack"));
    assert(!looks_like_title(p, Format::zip, "photos.zip"));
    assert(looks_like_title(p, Format::zip, "game.zip"));
    assert(!looks_like_title(p, Format::seven_zip, "photos.7z"));
    assert(looks_like_title(p, Format::seven_zip, "game.7z"));
    assert(looks_like_title(p, Format::zip, "wrapped.zip"));
    assert(looks_like_title(p, Format::seven_zip, "wrapped.7z"));
    auto tree = diagnose_tree(root);
    assert(tree.size() == 4);
    int reads = 0;
    auto reader = p.reader;
    p.reader = [&](auto &name) {
        ++reads;
        return reader(name);
    };
    DiagnosticCache cache;
    auto scan = [&](auto name, uint64_t size, int64_t mtime) {
        cache.begin();
        auto d = cache.get(p, root.string(), Format::zip, name, size, mtime);
        cache.end();
        return d;
    };
    assert(scan("game.zip", 100, 1)->incomplete());
    int first = reads;
    assert(scan("game.zip", 100, 1)->incomplete() && reads == first);
    assert(scan("game.zip", 101, 1)->incomplete() && reads > first);
    first = reads;
    assert(scan("game.zip", 101, 2)->incomplete() && reads > first);
    assert(!scan("photos.zip", 100, 1));
    first = reads;
    assert(!scan("photos.zip", 100, 1) && reads == first);
    cache.begin();
    cache.end();
    assert(cache.entries.empty());
}
void discovery_tests(const fs::path &root) {
    // The unrelated emulator directory seen on Xbox must never become a BotW diagnosis.
    auto unrelated = root / "Nativra/games/1145360";
    write(unrelated, "content/unrelated.bin");
    write(unrelated, "code/app.xml", "<title_id>0005000012345678</title_id>");
    assert(!looks_unpacked(filesystem_probe(unrelated), "1145360"));
    assert(!game_name("1145360") && !game_name("0005000012345678"));
    auto metadata = root / "metadata";
    for (auto family : {"00050000", "0005000e", "0005000c"})
        for (auto region : {"101c9300", "101c9400", "101c9500"}) {
            std::string id = std::string(family) + region;
            assert(game_name(id));
            for (auto path : {"code/app.xml", "meta/meta.xml"}) {
                fs::remove_all(metadata);
                write(metadata, path, "<title_id>" + id + "</title_id>");
                assert(looks_unpacked(filesystem_probe(metadata), "metadata"));
            }
        }
    auto partial = root / "BotW-partial";
    write(partial, "code/cos.xml");
    auto complete = root / "NXbox/games/BotW";
    write(complete, "code/U-King.rpx");
    write(complete, "code/cos.xml");
    write(complete, "code/app.xml", "<title_version>00d0</title_version>");
    write(complete, "meta/meta.xml", "<title_id>00050000101c9400</title_id>");
    write(complete, "content/Pack/Bootup.pack");
    auto too_deep = root / "one/two/three/four/BotW";
    write(too_deep, "code/cos.xml");
    auto diagnoses = diagnose_tree(root);
    assert(std::none_of(diagnoses.begin(), diagnoses.end(), [&](auto &d) {
        return d.folder == unrelated.generic_string() || d.folder == too_deep.generic_string();
    }));
    assert(std::any_of(diagnoses.begin(), diagnoses.end(),
                       [&](auto &d) { return d.folder == complete.generic_string() && !d.incomplete(); }));
    std::vector<fs::path> selectable;
    for (auto &candidate : {partial, unrelated, complete, too_deep})
        if (usable_source(diagnoses, candidate.generic_string(), Format::folder))
            selectable.push_back(candidate);
    assert(selectable == std::vector<fs::path>{complete});
    assert(!usable_source(diagnoses, complete.generic_string(), Format::zip, "other.zip"));
    auto second = root / "BotW-second";
    fs::copy(complete, second, fs::copy_options::recursive);
    diagnoses = diagnose_tree(root);
    selectable.clear();
    for (auto &candidate : {partial, complete, second})
        if (usable_source(diagnoses, candidate.generic_string(), Format::folder))
            selectable.push_back(candidate);
    assert((selectable == std::vector<fs::path>{complete, second}));
    fs::remove(complete / "content/Pack/Bootup.pack");
    diagnoses = diagnose_tree(root);
    assert(!usable_source(diagnoses, complete.generic_string(), Format::folder));
}
void chord_input_tests() {
    uwp::HoldChord chord;
    assert(!chord.update(uwp::menu | uwp::a, 0) && chord.guest_buttons == uwp::a);
    assert(!chord.update(uwp::menu | uwp::view, .2) && !chord.guest_buttons);
    assert(!chord.update(uwp::menu | uwp::view, .69) && !chord.guest_buttons);
    assert(chord.update(uwp::menu | uwp::view, .71) && !chord.guest_buttons);
    assert(!chord.update(uwp::menu, .8) && !chord.guest_buttons);
    chord.update(0, .9);
    chord = {};
    chord.update(uwp::view | uwp::menu, 0);
    assert(!chord.update(0, .1) && chord.guest_buttons == (uwp::view | uwp::menu));
    assert(!chord.update(0, .2) && !chord.guest_buttons);
    for (auto button : {uwp::menu, uwp::view}) {
        chord = {};
        assert(!chord.update(button, 0) && !chord.guest_buttons);
        assert(!chord.update(0, .1) && chord.guest_buttons == button);
        assert(!chord.update(0, .12) && chord.guest_buttons == button);
        assert(!chord.update(0, .16) && !chord.guest_buttons);
        chord = {};
        chord.update(button, 0);
        assert(!chord.update(button, .51) && chord.guest_buttons == button);
        assert(!chord.update(button, .6) && chord.guest_buttons == button);
        chord.update(0, .7);
        assert(!chord.guest_buttons);
    }
}
void dedupe_and_log_tests() {
    // Same game via a picked folder and a drive path: counted once.
    std::vector<ItemKey> keys{{"E:\\NXbox\\games\\BotW\\", Format::folder, "", {}},
                              {"e:/nxbox/games/botw", Format::folder, "", {}},
                              {"E:\\a.wua", Format::wua, "a.wua", {}},
                              {"E:\\b.wua", Format::wua, "b.wua", {}}};
    assert((distinct_items(keys) == std::vector<size_t>{0, 2, 3}));
    // A folder that is a layer of a layered item is the same game as that item.
    keys = {{"E:\\base", Format::folder, "", {"E:\\base", "E:\\update"}},
            {"E:\\update", Format::folder, "", {}},
            {"F:\\other", Format::folder, "", {}}};
    assert((distinct_items(keys) == std::vector<size_t>{0, 2}));
    LogMemo memo;
    assert(memo.changed("E:\\game", "valid") && !memo.changed("E:\\game", "valid"));
    assert(memo.changed("E:\\game", "incomplete") && memo.changed("E:\\game", "valid"));
    memo.drive_list("E:|");
    assert(memo.changed("E:\\game", "valid"));
    memo.drive_list("E:|");
    assert(!memo.changed("E:\\game", "valid"));
    memo.drive_list("E:|F:|");
    assert(memo.changed("E:\\game", "valid"));
}
int main() {
    dedupe_and_log_tests();
    auto root =
        fs::temp_directory_path() /
        ("w13-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{root};
    recognition_tests(root / "recognition");
    chord_input_tests();
    discovery_tests(root / "discovery");
    auto empty = root / "Zelda-empty";
    fs::create_directories(empty);
    exact(diagnose(filesystem_probe(empty), empty.string(), Format::folder),
          {{"code/U-King.rpx", IssueKind::missing},
           {"code/app.xml", IssueKind::missing},
           {"code/cos.xml", IssueKind::missing},
           {"meta/meta.xml", IssueKind::missing},
           {"content/", IssueKind::missing},
           {"content/Pack/Bootup.pack", IssueKind::missing},
           {"aoc/", IssueKind::optional}});
    auto folder = root / "BotW-partial";
    write(folder, "code/app.xml", "<app><title_version>0x0061</title_version></app>");
    write(folder, "meta/meta.xml", "<menu><title_id>0005000012345678</title_id></menu>");
    fs::create_directories(folder / "content");
    auto d = diagnose(filesystem_probe(folder), folder.string(), Format::folder);
    exact(d, {{"code/U-King.rpx", IssueKind::missing},
              {"code/cos.xml", IssueKind::missing},
              {"content/", IssueKind::missing},
              {"content/Pack/Bootup.pack", IssueKind::missing},
              {"code/U-King.rpx", IssueKind::version},
              {"meta/meta.xml", IssueKind::title},
              {"aoc/", IssueKind::optional}});
    assert(d.issues[4].value == 97);
    assert(issue_text(d.issues[4], false).find(L"v97 (0x0061)") != std::wstring::npos);
    write(folder, "code/U-King.rpx");
    write(folder, "code/cos.xml");
    write(folder, "content/Pack/Bootup.pack");
    write(folder, "code/app.xml", "<title_version>00d0</title_version>");
    for (auto id : {"00050000101c9400", "00050000101c9500", "00050000101c9300"}) {
        write(folder, "meta/meta.xml", std::string("<title_id>") + id + "</title_id>");
        auto valid = diagnose(filesystem_probe(folder), folder.string(), Format::folder);
        exact(valid, {{"aoc/", IssueKind::optional}});
        assert(!valid.incomplete());
    }
    auto update = root / "update";
    write(update, "code/app.xml", "<title_version>00d0</title_version>");
    write(folder, "code/app.xml", "<title_version>0061</title_version>");
    exact(diagnose(layered_probe({filesystem_probe(folder), filesystem_probe(update)}), folder.string(),
                   Format::folder),
          {{"aoc/", IssueKind::optional}});
    auto archive = root / "archive";
    fs::create_directories(archive);
    for (auto name : {"dump.wua", "dump.zip", "dump.7z"}) {
        write(archive, name, "truncated");
        exact(diagnose(filesystem_probe(archive), archive.string(),
                       *file_format(fs::path(name).extension().string()), name),
              {{name, IssueKind::incomplete}});
    }
    write(root / "a/b/c/Zelda-found", "code/cos.xml");
    write(root / "a/b/c/d/Zelda-too-deep", "code/cos.xml");
    auto all = diagnose_tree(root);
    // Depth 4 is found (one level under X:\NXbox\games\BotW); depth 5 stays out.
    assert(std::any_of(all.begin(), all.end(), [](auto &v) { return v.folder.ends_with("Zelda-found"); }));
    assert(std::none_of(all.begin(), all.end(), [](auto &v) { return v.folder.ends_with("Zelda-too-deep"); }));
    write(root / "a/Zelda-depth2", "code/cos.xml");
    all = diagnose_tree(root);
    assert(std::any_of(all.begin(), all.end(), [](auto &v) { return v.folder.ends_with("Zelda-depth2"); }));
    assert(game_name("BotW US") && game_name("Zelda") && game_name("00050000101C9400"));
    assert(!game_name("photos"));
    uwp::HoldChord hold;
    assert(!hold.update(uwp::view | uwp::menu, 1));
    assert(!hold.update(uwp::view | uwp::menu, 1.49));
    assert(hold.update(uwp::view | uwp::menu, 1.5));
    assert(!hold.update(uwp::view | uwp::menu, 2));
    assert(!hold.update(uwp::menu, 2.1));
    assert(!hold.update(uwp::view | uwp::menu, 3));
    assert(!hold.update(uwp::view | uwp::menu, 4));
    hold.update(0, 4.1);
    assert(!hold.update(uwp::view | uwp::menu, 5));
    assert(hold.update(uwp::view | uwp::menu, 5.5));
    assert(uwp::cycle_scale(1, 1.5f) == 1.5f && uwp::cycle_scale(1.5f, 1.5f) == 1);
    assert(uwp::cycle_scale(1.5f, 3) == 2 && uwp::cycle_scale(2, 3) == 3 && uwp::cycle_scale(3, 3) == 1);
    uwp::quick_menu.paused = true;
    auto waiter = std::async(std::launch::async, [] { uwp::wait_for_guest_resume(); });
    assert(waiter.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    uwp::quick_menu.paused = false;
    assert(waiter.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    waiter.get();
    std::cout << "PASS: exact folder/archive diagnostics, depth, held chord and scale caps\n";
}
