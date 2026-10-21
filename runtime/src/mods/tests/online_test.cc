// SPDX-License-Identifier: MIT
#include "../catalog.h"
#include "../gamebanana.h"
#include "../json_min.h"
#include "../zip_install.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <zlib.h>
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(#x);                                                                    \
    } while (0)
namespace fs = std::filesystem;
template <class F> void rejects(F f) {
    bool failed = false;
    try {
        f();
    } catch (...) {
        failed = true;
    }
    CHECK(failed);
}
void append(std::vector<unsigned char> &out, uint32_t n, int bytes) {
    for (int i = 0; i < bytes; ++i)
        out.push_back(static_cast<unsigned char>(n >> (8 * i)));
}
struct Item {
    std::string name, text;
    bool deflate = false;
    uint32_t attributes = 0;
};
void zip(const fs::path &path, const std::vector<Item> &items) {
    std::vector<unsigned char> out, central;
    for (auto &item : items) {
        std::vector<unsigned char> data(item.text.begin(), item.text.end());
        if (item.deflate) {
            z_stream z{};
            CHECK(deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) ==
                  Z_OK);
            std::vector<unsigned char> compressed(compressBound(data.size()));
            z.next_in = data.data();
            z.avail_in = data.size();
            z.next_out = compressed.data();
            z.avail_out = compressed.size();
            CHECK(deflate(&z, Z_FINISH) == Z_STREAM_END);
            compressed.resize(z.total_out);
            deflateEnd(&z);
            data = compressed;
        }
        uint32_t offset = out.size(),
                 crc = crc32(0, reinterpret_cast<const Bytef *>(item.text.data()), item.text.size());
        append(out, 0x04034b50, 4);
        append(out, 20, 2);
        append(out, 0, 2);
        append(out, item.deflate ? 8 : 0, 2);
        append(out, 0, 4);
        append(out, crc, 4);
        append(out, data.size(), 4);
        append(out, item.text.size(), 4);
        append(out, item.name.size(), 2);
        append(out, 0, 2);
        out.insert(out.end(), item.name.begin(), item.name.end());
        out.insert(out.end(), data.begin(), data.end());
        append(central, 0x02014b50, 4);
        append(central, 0x314, 2);
        append(central, 20, 2);
        append(central, 0, 2);
        append(central, item.deflate ? 8 : 0, 2);
        append(central, 0, 4);
        append(central, crc, 4);
        append(central, data.size(), 4);
        append(central, item.text.size(), 4);
        append(central, item.name.size(), 2);
        append(central, 0, 2);
        append(central, 0, 2);
        append(central, 0, 2);
        append(central, 0, 2);
        append(central, item.attributes, 4);
        append(central, offset, 4);
        central.insert(central.end(), item.name.begin(), item.name.end());
    }
    auto offset = out.size();
    out.insert(out.end(), central.begin(), central.end());
    append(out, 0x06054b50, 4);
    append(out, 0, 2);
    append(out, 0, 2);
    append(out, items.size(), 2);
    append(out, items.size(), 2);
    append(out, central.size(), 4);
    append(out, offset, 4);
    append(out, 0, 2);
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char *>(out.data()), out.size());
}
int main() {
    auto v = mods::json::parse(
        R"({"str":"a\n\t\b\f\r\/\\\"\u00e9\ud83d\ude80","a":[1,-2.5e2,true,false,null],"unknown":{}})");
    CHECK(v["str"].string() == "a\n\t\b\f\r/\\\"é🚀");
    CHECK(v["a"].array().size() == 5 && v["a"].array()[1].number() == -250);
    CHECK(v["a"].array()[2].boolean() && !v["missing"].boolean());
    for (auto s : {"", "[1,]", "{\"a\":}", "01", "1e", "true false", "1e9999", "\"\\q\""})
        rejects([&] { mods::json::parse(s); });
    // Lone surrogates become U+FFFD instead of failing the document.
    for (auto s : {"\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u0041\""})
        CHECK(mods::json::parse(s).string() == "\xEF\xBF\xBD");
    // A malformed record is skipped; the rest of the page survives.
    auto mixed = mods::parse_gamebanana(
        R"({"_aMetadata":{"_nRecordCount":2,"_bIsComplete":true},"_aRecords":[{"_idRow":-1,"_sName":"bad"},{"_idRow":7,"_sName":"ok"}]})");
    CHECK(mixed.records.size() == 1 && mixed.records[0].id == 7);
    auto page = mods::parse_gamebanana(
        R"({"_aMetadata":{"_nRecordCount":33,"_bIsComplete":false},"_aRecords":[{"_idRow":42,"_sName":"PT \u00e9","_aRootCategory":{"_sName":"Audio"},"_aSubmitter":{"_sName":"Author"},"_aPreviewMedia":{"_aImages":[]},"extra":null}]})");
    CHECK(page.count == 33 && !page.complete && page.records.size() == 1);
    CHECK(page.records[0].id == 42 && page.records[0].name == "PT é" && page.records[0].category == "Audio" &&
          page.records[0].author == "Author");
    auto files = mods::parse_gamebanana_files(
        R"({"_aFiles":[{"_sFile":"mod.ZIP","_nFilesize":100,"_sDownloadUrl":"https://example.org/a.zip","_sMd5Checksum":"abcd"},{"_sFile":"mod.bnp","_sDownloadUrl":"https://example.org/a.bnp"}]})");
    CHECK(files.size() == 2 && files[0].size == 100 && files[0].md5 == "abcd" && files[0].supported() &&
          !files[1].supported());
    rejects([] { mods::parse_gamebanana("{}"); });
    rejects([] { mods::parse_gamebanana_files("bad"); });
    CHECK(mods::percent_encode("é &/?#") == "%C3%A9%20%26%2F%3F%23");
    CHECK(mods::gamebanana_url(2).find("_nPage=2&_aFilters[Generic_Game]=5866") != std::string::npos);
    CHECK(mods::gamebanana_url(3, "a &é").find("_sSearchString=a%20%26%C3%A9&_nPage=3") != std::string::npos);
    CHECK(mods::gamebanana_files_url(42) == "https://gamebanana.com/apiv11/Mod/42/DownloadPage");
    CHECK(mods::https_zip_url("https://example.org/A.ZIP?token=x"));
    CHECK(!mods::https_zip_url("http://example.org/a.zip"));
    CHECK(!mods::https_zip_url("https:///a.zip"));
    auto root = fs::temp_directory_path() / "botw-w9-online-test";
    fs::remove_all(root);
    fs::create_directories(root);
    struct Guard {
        fs::path p;
        ~Guard() { fs::remove_all(p); }
    } guard{root};
    auto archive = root / "test.zip", installed = root / "mods";
    zip(archive, {{"release/nested/mod/content/Message/test.txt", "translation", false},
                  {"release/nested/mod/aoc/Sound/voice.bin", std::string(160000, 'x'), true}});
    auto result = mods::install_zip(archive, installed, "My mod");
    CHECK(!result.graphics && fs::file_size(result.path / "content/Message/test.txt") == 11 &&
          fs::file_size(result.path / "aoc/Sound/voice.bin") == 160000);
    auto second = mods::install_zip(archive, installed, "My mod");
    CHECK(second.path != result.path);
    for (auto name : {"../escape", "/absolute", "C:/drive", "a/../../escape", "a\\..\\escape", "content/aux",
                      "content/a:stream"}) {
        zip(archive, {{name, "bad"}});
        rejects([&] { mods::install_zip(archive, installed, "Bad"); });
        CHECK(!fs::exists(installed / "Bad"));
    }
    zip(archive, {{"content/link", "../outside", false, 0120777u << 16}});
    rejects([&] { mods::install_zip(archive, installed, "Link"); });
    zip(archive, {{"unrelated/readme.txt", "none"}});
    rejects([&] { mods::install_zip(archive, installed, "Empty"); });
    zip(archive, {{"pack/rules.txt", "[Definition]", true}});
    auto graphics = mods::install_zip(archive, installed, "Graphics");
    CHECK(graphics.graphics && fs::exists(graphics.path / "rules.txt"));
    mods::ModCatalog catalog;
    catalog.load(installed, {});
    CHECK(catalog.list().size() == 3);
    for (auto &entry : catalog.list()) {
        CHECK(!entry.enabled);
        if (entry.kind == mods::Kind::graphics) {
            CHECK(entry.locked);
            CHECK(!catalog.setEnabled(entry.id, true));
        }
    }
    CHECK(fs::is_empty(installed / ".downloads"));
    // Corrupt CRC must roll back extraction completely.
    zip(archive, {{"content/test", "test"}});
    {
        std::fstream f(archive, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(30 + 12);
        f.put('X');
    }
    rejects([&] { mods::install_zip(archive, installed, "Corrupt"); });
    CHECK(!fs::exists(installed / "Corrupt") && fs::is_empty(installed / ".downloads"));
    // Reject entry-count and expansion quotas before touching output files.
    zip(archive, {{"content/a", "a"}, {"content/b", "b"}});
    {
        std::fstream f(archive, std::ios::binary | std::ios::in | std::ios::out);
        std::string bytes((std::istreambuf_iterator<char>(f)), {});
        size_t at = bytes.find("PK\1\2");
        CHECK(at != std::string::npos);
        f.seekp(static_cast<std::streamoff>(at + 24));
        const char size[] = {'\0', '\0', '\0', char(0x90)};
        f.write(size, 4);
        at = bytes.find("PK\1\2", at + 4);
        CHECK(at != std::string::npos);
        f.seekp(static_cast<std::streamoff>(at + 24));
        f.write(size, 4);
    }
    rejects([&] { mods::install_zip(archive, installed, "Quota"); });
    zip(archive, {{"content/a", "a"}});
    {
        std::fstream f(archive, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(-14, std::ios::end);
        const char count[] = {char(0x21), char(0x4e)};
        f.write(count, 2);
        f.write(count, 2);
    }
    rejects([&] { mods::install_zip(archive, installed, "Count"); });
    CHECK(fs::is_empty(installed / ".downloads"));
    std::cout << "json_min: escapes, surrogates, malformed input passed\ngamebanana: mapping, HTTPS, URL "
                 "encoding passed\nzip: stored, deflate, nested root, traversal, symlink, CRC rollback, "
                 "graphics detection passed\n";
}
