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
    write(root, "photos.7z", seven_fixture("holiday/photo.jpg"));
    write(root, "game.7z", seven_fixture("dump/content/Bootup.pack"));
    write(root, "random.wud", "unrelated");
    assert(!looks_like_title(p, Format::zip, "photos.zip"));
    assert(looks_like_title(p, Format::zip, "game.zip"));
    assert(!looks_like_title(p, Format::seven_zip, "photos.7z"));
    assert(looks_like_title(p, Format::seven_zip, "game.7z"));
    assert(!looks_like_title(p, Format::wud, "random.wud"));
    auto tree = diagnose_tree(root);
    assert(tree.size() == 2);
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
    auto disc = p;
    disc.reader = [](auto &) { return uwp::formats::Reader{0x5d3a00000ull, {}}; };
    cache.begin();
    auto d = cache.get(disc, root.string(), Format::wud, "game.wud", 0x5d3a00000ull, 1);
    assert(d && d->issues.size() == 2);
    assert(issue_text(d->issues[0], false).starts_with(L"Keys missing"));
    assert(d->issues[0].path == "game.key / keys.txt");
    write(root, "game.key");
    write(root, "otp.bin");
    assert(!cache.get(disc, root.string(), Format::wud, "game.wud", 0x5d3a00000ull, 1)->incomplete());
    cache.end();
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
    write(unrelated, "title.tmd", "unrelated");
    assert(!looks_nus(filesystem_probe(unrelated), "1145360"));
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
    auto too_deep = root / "one/two/three/BotW";
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
int main() {
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
    auto nus = root / "cdn";
    std::string tmd(0xb04 + 3 * 0x30, '\0');
    auto put = [&](size_t at, uint64_t value, size_t n) {
        for (size_t i = 0; i < n; ++i)
            tmd[at + n - i - 1] = char(value >> (8 * i));
    };
    put(0x1de, 3, 2);
    for (unsigned i = 0; i < 3; ++i) {
        auto at = 0xb04 + i * 0x30;
        put(at, i + 1, 4);
        put(at + 4, i, 2);
        put(at + 6, i == 1 ? 2 : 0, 2);
        put(at + 8, 4, 8);
    }
    write(nus, "title.tmd", tmd);
    write(nus, "00000001.app", "1234");
    write(nus, "00000003.app", "12");
    exact(diagnose(filesystem_probe(nus), nus.string(), Format::nus),
          {{"title.tik", IssueKind::missing},
           {"title.cert", IssueKind::missing},
           {"00000002.app", IssueKind::missing},
           {"00000002.h3", IssueKind::missing},
           {"00000003.app", IssueKind::wrong_size}});
    write(nus, "title.tmd", "short");
    exact(diagnose(filesystem_probe(nus), nus.string(), Format::nus), {{"title.tik", IssueKind::missing},
                                                                       {"title.cert", IssueKind::missing},
                                                                       {"title.tmd", IssueKind::incomplete}});
    auto disc = root / "disc";
    fs::create_directories(disc);
    for (auto format : {Format::wud, Format::wux}) {
        exact(diagnose(filesystem_probe(disc), disc.string(), format, "game.wud"),
              {{"game.key / keys.txt", IssueKind::keys}, {"otp.bin / common.key", IssueKind::keys}});
    }
    write(disc, "custom.key");
    write(disc, "common.key");
    exact(diagnose(filesystem_probe(disc), disc.string(), Format::wux, "custom.wux"), {});
    for (auto name : {"dump.wua", "dump.zip", "dump.7z"}) {
        write(disc, name, "truncated");
        exact(diagnose(filesystem_probe(disc), disc.string(),
                       *file_format(fs::path(name).extension().string()), name),
              {{name, IssueKind::incomplete}});
    }
    write(root / "a/b/Zelda-found", "code/cos.xml");
    write(root / "a/b/c/Zelda-too-deep", "code/cos.xml");
    auto all = diagnose_tree(root);
    // Depth 3 is X:\NXbox\games\BotW (NXbox FTP uploads); depth 4 stays out.
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
    std::cout << "PASS: exact folder/NUS/disc/archive diagnostics, depth, held chord and scale caps\n";
}
