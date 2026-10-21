// SPDX-License-Identifier: MIT
#include "../formats/layout.h"
#include "../qr.h"
#include "../setup.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
using namespace uwp::formats;
void put(Bytes &b, size_t p, uint64_t v, size_t n = 4, bool little = false) {
    assert(p + n <= b.size());
    for (size_t i = 0; i < n; ++i)
        b[p + (little ? i : n - 1 - i)] = uint8_t(v >> (i * 8));
}
void copy(Bytes &b, size_t p, std::span<const uint8_t> v) {
    assert(p + v.size() <= b.size());
    std::copy(v.begin(), v.end(), b.begin() + p);
}
Bytes output(const Entry &entry) {
    Bytes result;
    entry.extract([&](auto b) { result.insert(result.end(), b.begin(), b.end()); });
    assert(result.size() == entry.size);
    return result;
}
template <class F> void rejects(F f) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}
Bytes fst(std::string name, uint64_t size, uint16_t index = 0, bool hashed = false) {
    Bytes b(0x20 + 0x20 * (index + 1) + 32 + name.size() + 2);
    put(b, 0, 0x46535400);
    put(b, 4, 1);
    put(b, 8, index + 1);
    for (unsigned i = 0; i <= index; ++i) {
        put(b, 0x20 + i * 0x20, 2);
        put(b, 0x24 + i * 0x20, hashed ? 2 : 1);
        put(b, 0x34 + i * 0x20, hashed ? 2 : 0, 1);
    }
    size_t tree = 0x20 + 0x20 * (index + 1);
    put(b, tree, 0x01000000);
    put(b, tree + 8, 2);
    put(b, tree + 16, 1);
    put(b, tree + 24, size);
    put(b, tree + 30, index, 2);
    std::copy(name.begin(), name.end(), b.begin() + tree + 33);
    return b;
}
void hashes() {
    std::string abc = "abc";
    auto data = reinterpret_cast<const uint8_t *>(abc.data());
    assert(packs::sha256_bytes(data, 3) ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    packs::Sha256 h;
    h.update(data, 1);
    h.update(data + 1, 2);
    assert(h.finish() == packs::sha256_bytes(data, 3));
    Bytes million(1000000, 'a');
    assert(packs::sha256_bytes(million.data(), million.size()) ==
           "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    Key key{}, iv{};
    for (unsigned i = 0; i < 16; ++i) {
        key[i] = uint8_t(i);
        iv[i] = uint8_t(15 - i);
    }
    Bytes plain(256);
    for (unsigned i = 0; i < 256; ++i)
        plain[i] = uint8_t(i);
    assert(aes_cbc(key, iv, aes_cbc(key, iv, plain, true)) == plain);
    // NIST SP800-38A, unrelated public test key, never Wii U key material.
    auto nist = *hex_key("2b7e151628aed2a6abf7158809cf4f3c");
    auto vector_iv = *hex_key("000102030405060708090a0b0c0d0e0f");
    auto vector_plain = *hex_key("6bc1bee22e409f96e93d7e117393172a");
    auto vector_cipher = *hex_key("7649abac8119b246cee98e9b12e9197d");
    assert(aes_cbc(nist, vector_iv, vector_plain, true) == Bytes(vector_cipher.begin(), vector_cipher.end()));
    std::string keys = " # ignored\n000102030405060708090a0b0c0d0e0f # "
                       "synthetic\ninvalid\nffffffffffffffffffffffffffffffff\r\n";
    auto parsed = parse_keys(std::span(reinterpret_cast<const uint8_t *>(keys.data()), keys.size()), false);
    assert(parsed.size() == 2 && parsed[0] == key);
    assert(!hex_key("not a key"));
    assert(!parse_common(Bytes(1024)));
    assert(!parse_common(Bytes(16)));
    assert(!parse_common(Bytes(1023)));
    Bytes otp(1024);
    copy(otp, 0xe0, key);
    assert(!parse_common(otp));
    auto synthetic_digest = packs::sha256_bytes(key.data(), key.size());
    assert(parse_common(otp, synthetic_digest) == key);
    otp[0xe1] ^= 1;
    assert(!parse_common(otp, synthetic_digest));
    // Positive OTP parsing uses a synthetic digest; no production common key is present.
    std::cout << "PASS: shared streaming SHA-256, AES-CBC NIST vector, keys.txt, wrong otp.bin rejected\n";
}
void wux_test() {
    Bytes image(64 + 3 * 64);
    put(image, 0, 0x30585557, 4, true);
    put(image, 4, 0x1099d02e, 4, true);
    put(image, 8, 64, 4, true);
    put(image, 16, 192, 8, true);
    put(image, 32, 1, 4, true);
    put(image, 36, 0, 4, true);
    put(image, 40, 1, 4, true);
    std::fill(image.begin() + 64, image.begin() + 128, 'a');
    std::fill(image.begin() + 128, image.begin() + 192, 'b');
    auto reader = disc_reader(memory_reader(image));
    auto data = reader.bytes(60, 72);
    assert(data.size() == 72);
    assert(data[0] == 'b' && data[4] == 'a' && data[68] == 'b');
    put(image, 32, 99, 4, true);
    rejects([&] { disc_reader(memory_reader(image)); });
    std::cout << "PASS: WUX index, deduplication, cross-sector and invalid index\n";
}
void wud_test() {
    Key disc{}, common{}, title{};
    disc[0] = 0x42;
    common[0] = 0x24;
    title[0] = 0x81;
    Bytes image(0xa0000);
    put(image, 0x10000, 0xcc549eb9);
    Bytes pt(0x8000);
    put(pt, 0, 0xcca6e67b);
    put(pt, 0x1c, 2);
    pt[0x800] = 'S';
    pt[0x801] = 'I';
    put(pt, 0x820, 4);
    pt[0x880] = 'G';
    pt[0x881] = 'M';
    put(pt, 0x8a0, 12);
    copy(image, 0x18000, aes_cbc(disc, {}, pt, true));
    Bytes ticket(0x350);
    ticket[0x1dc] = 0x12;
    ticket[0x1e3] = 0x34;
    Key iv{};
    std::copy_n(ticket.begin() + 0x1dc, 8, iv.begin());
    copy(ticket, 0x1bf, aes_cbc(common, iv, title, true));
    auto si_fst = fst("title.tik", ticket.size());
    // SI directory 01/title.tik.
    size_t tree = 0x40;
    si_fst.resize(0x40 + 48 + 14);
    put(si_fst, tree + 8, 3);
    put(si_fst, tree + 16, 0x01000001);
    put(si_fst, tree + 20, 0);
    put(si_fst, tree + 24, 3);
    put(si_fst, tree + 32, 4);
    put(si_fst, tree + 36, 0);
    put(si_fst, tree + 40, ticket.size());
    put(si_fst, tree + 46, 0, 2);
    std::fill(si_fst.begin() + tree + 48, si_fst.end(), 0);
    std::string names("\0"
                      "01\0title.tik\0",
                      14);
    std::copy(names.begin(), names.end(), si_fst.begin() + tree + 48);
    Bytes payload(300);
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = uint8_t(i);
    auto make_volume = [&](size_t base, Bytes f, Key key, Bytes data, bool hashed) {
        put(image, base, 0xcc93a4f5);
        put(image, base + 0x14, f.size());
        put(image, base + 0x18, 1);
        f.resize((f.size() + 15) / 16 * 16);
        copy(image, base + 0x8000, aes_cbc(key, {}, f, true));
        if (hashed) {
            Bytes block(0xfc00);
            copy(block, 0, data);
            Bytes hashes(0x400);
            auto digest = sha1(block);
            copy(hashes, 0, digest);
            Key block_iv{};
            std::copy_n(digest.begin(), 16, block_iv.begin());
            copy(image, base + 0x10000, aes_cbc(key, {}, hashes, true));
            copy(image, base + 0x10400, aes_cbc(key, block_iv, block, true));
        } else {
            data.resize(0x8000);
            copy(image, base + 0x10000, aes_cbc(key, {}, data, true));
        }
    };
    make_volume(0x20000, si_fst, disc, ticket, false);
    for (bool hashed : {false, true}) {
        auto gm_fst = fst("sample.bin", payload.size(), 0, hashed);
        make_volume(0x60000, gm_fst, title, payload, hashed);
        auto entries = wud(memory_reader(image), disc, common);
        assert(entries.size() == 1 && entries[0].path == "sample.bin" && output(entries[0]) == payload);
    }
    Bytes packed(0x8000);
    put(packed, 0, 0x30585557, 4, true);
    put(packed, 4, 0x1099d02e, 4, true);
    put(packed, 8, 0x8000, 4, true);
    put(packed, 16, image.size(), 8, true);
    std::vector<Bytes> sectors;
    for (size_t p = 0; p < image.size(); p += 0x8000) {
        Bytes sector(image.begin() + p, image.begin() + p + 0x8000);
        auto existing = std::find(sectors.begin(), sectors.end(), sector);
        size_t index = size_t(existing - sectors.begin());
        if (existing == sectors.end())
            sectors.push_back(std::move(sector));
        put(packed, 32 + p / 0x8000 * 4, index, 4, true);
    }
    for (auto &sector : sectors)
        packed.insert(packed.end(), sector.begin(), sector.end());
    auto from_wux = wud(memory_reader(packed), disc, common);
    assert(output(from_wux.at(0)) == payload);
    rejects([&] { wud(memory_reader(image), Key{}, common); });
    std::cout << "PASS: synthetic encrypted WUD partition table, SI ticket, GM raw and hashed clusters\n";
}
void fst_links_test() {
    // Update links refer to base files, even when their offsets fit a patch cluster.
    Bytes table(0x40 + 6 * 16);
    put(table, 0, 0x46535400);
    put(table, 4, 1);
    put(table, 8, 1);
    auto entry = [&](size_t index, uint8_t type, const std::string &name, uint32_t offset, uint32_t size) {
        size_t p = 0x40 + index * 16;
        put(table, p, (uint32_t(type) << 24) | uint32_t(table.size() - 0x40 - 6 * 16));
        put(table, p + 4, offset);
        put(table, p + 8, size);
        table.insert(table.end(), name.begin(), name.end());
        table.push_back(0);
    };
    entry(0, 1, "", 0, 6);
    entry(1, 0x80, "base.bin", 0, 4);
    entry(2, 0x81, "inherited", 0, 5);
    entry(3, 0x80, "child.bin", 0xffffffff, 4);
    entry(4, 0, "patched.bin", 0, 4);
    entry(5, 0, "root.bin", 0, 4);
    Bytes patch{'n', 'e', 'w', '!'};
    unsigned readers = 0;
    auto entries = Fst(table).entries([&](uint16_t cluster) {
        assert(cluster == 0);
        ++readers;
        return memory_reader(patch);
    });
    assert(readers == 2 && entries.size() == 2);
    assert(entries[0].path == "inherited/patched.bin" && output(entries[0]) == patch);
    assert(entries[1].path == "root.bin" && output(entries[1]) == patch);
    std::cout << "PASS: FST update links preserve base files and directory traversal\n";
}
void nus_test() {
    Key common{}, title{};
    common[2] = 1;
    title[3] = 2;
    Bytes ticket(0x350);
    ticket[0x1dc] = 1;
    Key iv{};
    iv[0] = 1;
    copy(ticket, 0x1bf, aes_cbc(common, iv, title, true));
    Bytes payload{'h', 'e', 'l', 'l', 'o'};
    auto table = fst("test.bin", payload.size(), 1);
    table.resize((table.size() + 15) / 16 * 16);
    Bytes tmd(0xb04 + 96);
    put(tmd, 0x1de, 2, 2);
    put(tmd, 0xb04, 0xaa);
    put(tmd, 0xb04 + 8, table.size(), 8);
    put(tmd, 0xb34, 0xbb);
    put(tmd, 0xb34 + 4, 1, 2);
    put(tmd, 0xb34 + 8, payload.size(), 8);
    auto first = aes_cbc(title, {}, table, true);
    Bytes padded = payload;
    padded.resize(16);
    iv = {};
    iv[1] = 1;
    auto second = aes_cbc(title, iv, padded, true);
    auto entries = nus(tmd, ticket, common,
                       [&](auto name) { return memory_reader(name == "000000aa.app" ? first : second); });
    assert(entries.size() == 1 && output(entries[0]) == payload);
    // A hashed .app validates H0 -> H1 -> H2 -> external .h3.
    Bytes block(0xfc00);
    copy(block, 0, payload);
    Bytes hashes(0x400);
    copy(hashes, 0, sha1(block));
    copy(hashes, 0x140, sha1(std::span(hashes).first(0x140)));
    copy(hashes, 0x280, sha1(std::span(hashes).subspan(0x140, 0x140)));
    auto top = sha1(std::span(hashes).subspan(0x280, 0x140));
    Bytes h3(top.begin(), top.end());
    Key block_iv{};
    std::copy_n(hashes.begin(), 16, block_iv.begin());
    auto hashed = aes_cbc(title, {}, hashes, true);
    auto cipher = aes_cbc(title, block_iv, block, true);
    hashed.insert(hashed.end(), cipher.begin(), cipher.end());
    put(tmd, 0xb34 + 6, 2, 2);
    put(tmd, 0xb34 + 8, hashed.size(), 8);
    auto open = [&](const std::string &name) {
        if (name == "000000aa.app")
            return memory_reader(first);
        if (name == "000000bb.h3")
            return memory_reader(h3);
        return memory_reader(hashed);
    };
    entries = nus(tmd, ticket, common, open);
    assert(output(entries.at(0)) == payload);
    h3[0] ^= 1;
    entries = nus(tmd, ticket, common, open);
    rejects([&] { output(entries.at(0)); });
    std::cout << "PASS: NUS synthetic ticket, content-index IV, hashed .app + .h3 hierarchy and corruption "
                 "rejection\n";
}
void wua_test() {
    Bytes data(65536);
    std::string value = "synthetic WUA";
    std::copy(value.begin(), value.end(), data.begin());
    Bytes compressed(ZSTD_compressBound(data.size()));
    auto n = ZSTD_compress(compressed.data(), compressed.size(), data.data(), data.size(), 3);
    assert(!ZSTD_isError(n));
    compressed.resize(n);
    Bytes image = compressed;
    size_t records = image.size();
    image.resize(image.size() + 40);
    put(image, records, 0, 8);
    put(image, records + 8, n - 1, 2);
    size_t names = image.size();
    std::string text("\x08"
                     "file.bin",
                     9);
    image.insert(image.end(), text.begin(), text.end());
    size_t tree = image.size();
    image.resize(image.size() + 32);
    put(image, tree, 0x7fffffff);
    put(image, tree + 4, 1);
    put(image, tree + 8, 1);
    put(image, tree + 16, 0x80000000);
    put(image, tree + 24, value.size());
    size_t footer = image.size();
    image.resize(image.size() + 144);
    put(image, footer, 0, 8);
    put(image, footer + 8, compressed.size(), 8);
    put(image, footer + 16, records, 8);
    put(image, footer + 24, 40, 8);
    put(image, footer + 32, names, 8);
    put(image, footer + 40, text.size(), 8);
    put(image, footer + 48, tree, 8);
    put(image, footer + 56, 32, 8);
    put(image, footer + 128, image.size(), 8);
    put(image, footer + 136, 0x61bf3a01);
    put(image, footer + 140, 0x169f52d6);
    auto digest = packs::sha256_bytes(image.data(), image.size());
    for (size_t i = 0; i < 32; ++i)
        image[footer + 96 + i] = uint8_t(std::stoul(digest.substr(i * 2, 2), nullptr, 16));
    auto entries = wua(memory_reader(image));
    assert(entries.size() == 1 && entries[0].path == "file.bin");
    auto unpacked = output(entries[0]);
    assert(std::string(unpacked.begin(), unpacked.end()) == value);
    put(image, footer + 48, UINT64_MAX, 8);
    rejects([&] { wua(memory_reader(image)); });
    std::cout << "PASS: synthetic compressed ZArchive/WUA, footer bounds\n";
}
void zip_test() {
    auto make = [](std::string path, bool deflate) {
        Bytes plain(150000);
        for (size_t i = 0; i < plain.size(); ++i)
            plain[i] = uint8_t(i * 13);
        Bytes packed = plain;
        if (deflate) {
            z_stream stream{};
            assert(deflateInit2(&stream, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK);
            packed.resize(compressBound(uLong(plain.size())));
            stream.next_in = plain.data();
            stream.avail_in = uInt(plain.size());
            stream.next_out = packed.data();
            stream.avail_out = uInt(packed.size());
            assert(::deflate(&stream, Z_FINISH) == Z_STREAM_END);
            packed.resize(stream.total_out);
            deflateEnd(&stream);
        }
        auto crc = crc32(0, plain.data(), uInt(plain.size()));
        Bytes image(30 + path.size());
        put(image, 0, 0x04034b50, 4, true);
        put(image, 8, deflate ? 8 : 0, 2, true);
        put(image, 26, path.size(), 2, true);
        std::copy(path.begin(), path.end(), image.begin() + 30);
        image.insert(image.end(), packed.begin(), packed.end());
        size_t directory = image.size();
        image.resize(image.size() + 46 + path.size());
        put(image, directory, 0x02014b50, 4, true);
        put(image, directory + 10, deflate ? 8 : 0, 2, true);
        put(image, directory + 16, crc, 4, true);
        put(image, directory + 20, packed.size(), 4, true);
        put(image, directory + 24, plain.size(), 4, true);
        put(image, directory + 28, path.size(), 2, true);
        std::copy(path.begin(), path.end(), image.begin() + directory + 46);
        size_t end = image.size();
        image.resize(image.size() + 22);
        put(image, end, 0x06054b50, 4, true);
        put(image, end + 8, 1, 2, true);
        put(image, end + 10, 1, 2, true);
        put(image, end + 12, end - directory, 4, true);
        put(image, end + 16, directory, 4, true);
        return std::pair{image, plain};
    };
    for (bool compressed : {false, true}) {
        auto [image, plain] = make("content/test.bin", compressed);
        auto entries = zip(memory_reader(image));
        assert(output(entries.at(0)) == plain);
        image[30 + 16] ^= 1;
        entries = zip(memory_reader(image));
        rejects([&] { output(entries.at(0)); });
    }
    auto [bad, payload] = make("../escape", false);
    rejects([&] { zip(memory_reader(bad)); });
    auto entry = [](std::string name, char c) {
        return Entry{name, 1, [c](const Sink &sink) {
                         uint8_t b = uint8_t(c);
                         sink(std::span(&b, 1));
                     }};
    };
    auto merged = merge_titles({entry("0005000012345678_v0/code/U-King.rpx", 'b'),
                                entry("0005000012345678_v0/content/base", 'b'),
                                entry("0005000e12345678_v208/code/U-King.rpx", 'u'),
                                entry("0005000c12345678_v80/content/dlc", 'd')});
    assert(merged.size() == 3);
    for (auto &file : merged) {
        if (file.path == "code/U-King.rpx")
            assert(output(file) == Bytes{'u'});
        if (file.path == "aoc/dlc")
            assert(output(file) == Bytes{'d'});
    }
    std::cout
        << "PASS: stored/deflated ZIP, CRC/traversal rejection, virtual base < update and DLC mapping\n";
}
void setup_test() {
    namespace fs = std::filesystem;
    using namespace uwp::setup;
    auto root =
        fs::temp_directory_path() /
        ("botw-setup-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(root);
    fs::create_directories(root / "one/Zelda/code");
    fs::create_directories(root / "one/Zelda/content");
    fs::create_directories(root / "one/Zelda/meta");
    std::ofstream(root / "one/Zelda/code/U-King.rpx") << "synthetic";
    fs::create_directories(root / "install");
    std::ofstream(root / "install/title.tmd") << "test";
    std::ofstream(root / "install/title.tik") << "test";
    for (auto ext : {".wua", ".wud", ".wux", ".zip", ".7z"})
        std::ofstream(root / (std::string("game") + ext)) << "synthetic";
    fs::create_directories(root / "a/b/deep/too-deep");
    std::ofstream(root / "a/b/deep/found.wux") << "x";
    std::ofstream(root / "a/b/deep/too-deep/ignored.wux") << "x";
    auto found = detect(root);
    assert(found.size() == 8);
    assert(std::any_of(found.begin(), found.end(),
                       [](auto &item) { return item.path.filename() == "found.wux"; }));
    assert(std::none_of(found.begin(), found.end(),
                        [](auto &item) { return item.path.filename() == "ignored.wux"; }));
    std::set<Format> formats;
    for (auto &item : found)
        formats.insert(item.format);
    assert(formats.size() == 7);
    fs::remove_all(root);
    assert(title_version("<app><title_version type=\"hexBinary\">0061</title_version></app>") == 97);
    assert(title_version("<title_version> 0x00d0 </title_version>") == 208);
    assert(!title_version("<title_version>garbage</title_version>"));
    for (auto mode : {Mode::ask, Mode::copy, Mode::external, Mode::off})
        assert(parse_mode(mode_name(mode) + "\n") == mode);
    assert(parse_mode("copy-invalid") == Mode::ask);
    State s;
    s.next();
    assert(s.step == Step::guide);
    for (int i = 0; i < 4; ++i)
        s.next();
    assert(s.step == Step::detect);
    s.found();
    s.hash("wrong");
    assert(s.step == Step::wrong_version && !s.verified);
    s.removed();
    assert(s.step == Step::detect && s.drive_removed);
    s.found();
    s.hash(expected_rpx);
    assert(s.step == Step::storage && s.verified);
    s.next();
    s.next();
    assert(s.step == Step::home);
    s.removed();
    assert(!s.verified && s.step == Step::detect);
    Space space{margin + 100, margin + 200, true};
    assert(space.fits(100) && !space.fits(101));
    assert(!Space{}.fits(0));
    s.mode = Mode::copy;
    assert(s.storage_focus(space, 100) == 1 && s.storage_focus(space, 101) == 0);
    Repeat repeat;
    assert(repeat.update(1, 1, 0) == 1);
    assert(repeat.update(1, 1, .39) == 0);
    assert(repeat.update(1, 1, .4) == 1);
    assert(repeat.update(1, 1, .51) == 0);
    assert(repeat.update(1, 1, .52) == 1);
    assert(gigabytes(42300000000, true) == L"42,3 GB");
    assert(!safe_path("../bad") && !safe_path("a/CON.txt") && !safe_path("C:/foo"));
    std::cout << "PASS: every format fixture, depth limit, setup transitions, wrong hash, drive removed, "
                 "app.xml, capacity, repeat\n";
}
void qr_test() {
    // Force byte-mode ECC M, mask 0 for a stable known vector; verify payload bits below.
    const std::string value = "https://wiiu.hacks.guide/";
    auto qr = qrcodegen::QrCode::encodeSegments(
        {qrcodegen::QrSegment::makeBytes(Bytes(value.begin(), value.end()))}, qrcodegen::QrCode::Ecc::MEDIUM,
        1, 5, 0, false);
    assert(qr.getVersion() == 2 && qr.getSize() == 25);
    // Independent v2 decoder: remove function modules, undo mask, decode the byte payload,
    // then check every Reed-Solomon syndrome (one block, 28 data + 16 parity codewords).
    std::vector<bool> bits;
    for (int right = 24; right >= 1; right -= 2) {
        if (right == 6)
            --right;
        for (int vert = 0; vert < 25; ++vert) {
            int y = ((right + 1) & 2) == 0 ? 24 - vert : vert;
            for (int j = 0; j < 2; ++j) {
                int x = right - j;
                // function modules: timing row, format row/column and alignment pattern.
                if (x == 6 || y == 6 || (x <= 8 && y <= 8) || (x >= 17 && y <= 8) || (x <= 8 && y >= 17) ||
                    (x >= 16 && x <= 20 && y >= 16 && y <= 20))
                    continue;
                bits.push_back(qr.getModule(x, y) ^ ((x + y) % 2 == 0));
            }
        }
    }
    auto read = [&](size_t p, size_t n) {
        unsigned result = 0;
        for (size_t i = 0; i < n; ++i)
            result = result * 2 + bits.at(p + i);
        return result;
    };
    assert(read(0, 4) == 4 && read(4, 8) == value.size());
    for (size_t i = 0; i < value.size(); ++i)
        assert(read(12 + i * 8, 8) == static_cast<unsigned char>(value[i]));
    assert(bits.size() == 359);
    auto multiply = [](unsigned a, unsigned b) {
        unsigned result = 0;
        for (unsigned i = 0; i < 8; ++i) {
            if (b & 1)
                result ^= a;
            b >>= 1;
            a = (a << 1) ^ ((a & 128) ? 0x11d : 0);
        }
        return result;
    };
    unsigned root = 1;
    for (int i = 0; i < 16; ++i) {
        unsigned syndrome = 0;
        for (size_t j = 0; j < 44; ++j)
            syndrome = multiply(syndrome, root) ^ read(j * 8, 8);
        assert(syndrome == 0);
        root = multiply(root, 2);
    }
    for (size_t i = 352; i < bits.size(); ++i)
        assert(!bits[i]);
    for (int version = 1; version <= 5; ++version) {
        auto fixed =
            qrcodegen::QrCode::encodeSegments({qrcodegen::QrSegment::makeBytes(Bytes{'a'})},
                                              qrcodegen::QrCode::Ecc::MEDIUM, version, version, 0, false);
        assert(fixed.getVersion() == version);
    }
    rejects([] {
        qrcodegen::QrCode::encodeSegments({qrcodegen::QrSegment::makeBytes(Bytes(1000, 'a'))},
                                          qrcodegen::QrCode::Ecc::MEDIUM, 1, 5, 0, false);
    });
    std::cout << "PASS: QR versions 1–5, byte-mode ECC M URL decoded, 16 Reed-Solomon syndromes verified\n";
}
int main() {
    hashes();
    wux_test();
    wud_test();
    fst_links_test();
    nus_test();
    wua_test();
    zip_test();
    setup_test();
    qr_test();
}
