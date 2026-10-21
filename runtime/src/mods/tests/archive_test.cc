// SPDX-License-Identifier: MIT
#include "../archive_install.h"
#include <archive.h>
#include <archive_entry.h>
#include <cassert>
#include <fstream>
#include <iostream>
#include <vector>
#include <zlib.h>
namespace fs = std::filesystem;
void seven(const fs::path &file, const char *name, bool link = false, bool dlc = false) {
    auto *a = archive_write_new();
    assert(archive_write_set_format_7zip(a) == ARCHIVE_OK);
    assert(archive_write_open_filename(a, file.c_str()) == ARCHIVE_OK);
    auto *e = archive_entry_new();
    archive_entry_set_pathname(e, name);
    archive_entry_set_perm(e, 0644);
    if (link) {
        archive_entry_set_filetype(e, AE_IFLNK);
        archive_entry_set_symlink(e, "../../outside");
        archive_entry_set_size(e, 0);
    } else {
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_size(e, 4);
    }
    assert(archive_write_header(a, e) == ARCHIVE_OK);
    if (!link)
        assert(archive_write_data(a, "test", 4) == 4);
    archive_entry_free(e);
    if (dlc) {
        e = archive_entry_new();
        archive_entry_set_pathname(e, "wrapper/aoc/content/dlc.txt");
        archive_entry_set_perm(e, 0644);
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_size(e, 4);
        assert(archive_write_header(a, e) == ARCHIVE_OK);
        assert(archive_write_data(a, "test", 4) == 4);
        archive_entry_free(e);
    }
    assert(archive_write_close(a) == ARCHIVE_OK);
    assert(archive_write_free(a) == ARCHIVE_OK);
}
void append(std::vector<unsigned char> &v, uint64_t n, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i)
        v.push_back(uint8_t(n >> (8 * i)));
}
void header(std::ofstream &out, std::vector<unsigned char> body) {
    auto crc = crc32(0, body.data(), uInt(body.size()));
    unsigned char c[] = {uint8_t(crc), uint8_t(crc >> 8)};
    out.write(reinterpret_cast<char *>(c), 2);
    out.write(reinterpret_cast<char *>(body.data()), body.size());
}
void rar(const fs::path &file, const std::string &name) {
    std::ofstream out(file, std::ios::binary);
    out.write("Rar!\x1a\x07\0", 7);
    std::vector<unsigned char> main{0x73};
    append(main, 0, 2);
    append(main, 13, 2);
    append(main, 0, 6);
    header(out, main);
    std::vector<unsigned char> entry{0x74};
    append(entry, 0x8000, 2);
    append(entry, 32 + name.size(), 2);
    append(entry, 4, 4);
    append(entry, 4, 4);
    append(entry, 3, 1);
    append(entry, crc32(0, reinterpret_cast<const unsigned char *>("test"), 4), 4);
    append(entry, 0, 4);
    append(entry, 20, 1);
    append(entry, 0x30, 1);
    append(entry, name.size(), 2);
    append(entry, 0100644, 4);
    entry.insert(entry.end(), name.begin(), name.end());
    header(out, entry);
    out.write("test", 4);
    std::vector<unsigned char> end{0x7b};
    append(end, 0, 2);
    append(end, 7, 2);
    header(out, end);
}
int main() {
    auto root = fs::temp_directory_path() / "w15-archive-fixture";
    fs::remove_all(root);
    fs::create_directories(root);
    seven(root / "input.7z", "wrapper/content/test.txt");
    auto a = mods::install_archive(root / "input.7z", root / "mods", "Seven");
    assert(fs::file_size(a.path / "content" / "test.txt") == 4);
    seven(root / "dlc.7z", "wrapper/content/test.txt", false, true);
    auto dlc = mods::install_archive(root / "dlc.7z", root / "mods", "DLC");
    assert(!dlc.graphics);
    assert(fs::file_size(dlc.path / "content" / "test.txt") == 4);
    assert(fs::file_size(dlc.path / "aoc" / "content" / "dlc.txt") == 4);
    rar(root / "input.rar", "wrapper/content/test.txt");
    auto b = mods::install_archive(root / "input.rar", root / "mods", "Rar");
    assert(fs::file_size(b.path / "content" / "test.txt") == 4);
    seven(root / "bad.7z", "../outside");
    bool failed = false;
    try {
        mods::install_archive(root / "bad.7z", root / "mods", "Bad");
    } catch (...) {
        failed = true;
    }
    assert(failed && !fs::exists(root / "outside"));
    seven(root / "link.7z", "content/link", true);
    failed = false;
    try {
        mods::install_archive(root / "link.7z", root / "mods", "Link");
    } catch (...) {
        failed = true;
    }
    assert(failed);
    rar(root / "bad.rar", "../outside");
    failed = false;
    try {
        mods::install_archive(root / "bad.rar", root / "mods", "Bad Rar");
    } catch (...) {
        failed = true;
    }
    assert(failed);
    assert(fs::is_empty(root / "mods" / ".downloads"));
    fs::remove_all(root);
    std::cout << "archive tests passed\n";
}
