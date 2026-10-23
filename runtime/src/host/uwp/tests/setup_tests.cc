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
    std::cout << "PASS: shared streaming SHA-256\n";
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
    for (auto ext : {".wua", ".zip", ".7z"})
        std::ofstream(root / (std::string("game") + ext)) << "synthetic";
    fs::create_directories(root / "a/b/c/deep/too-deep");
    std::ofstream(root / "a/b/c/deep/found.wua") << "x";
    std::ofstream(root / "a/b/c/deep/too-deep/ignored.wua") << "x";
    auto found = detect(root);
    assert(found.size() == 5);
    assert(std::any_of(found.begin(), found.end(),
                       [](auto &item) { return item.path.filename() == "found.wua"; }));
    assert(std::none_of(found.begin(), found.end(),
                        [](auto &item) { return item.path.filename() == "ignored.wua"; }));
    std::set<Format> formats;
    for (auto &item : found)
        formats.insert(item.format);
    assert(formats.size() == 4);
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
    assert(s.step == Step::buttons);
    s.back();
    assert(s.step == Step::storage);
    s.next();
    s.next();
    assert(s.step == Step::automatic);
    s.back();
    assert(s.step == Step::buttons);
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
    const std::string value = "https://example.com/help";
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
    wua_test();
    zip_test();
    setup_test();
    qr_test();
}
