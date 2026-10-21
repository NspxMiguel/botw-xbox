// SPDX-License-Identifier: MIT
#include "archive_install.h"
#ifdef BOTW_MOD_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif
#include <algorithm>
#include <array>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
namespace mods {
namespace fs = std::filesystem;
namespace {
std::string lower(std::string s) {
    for (auto &c : s)
        if (c >= 'A' && c <= 'Z')
            c += 32;
    return s;
}
#ifdef BOTW_MOD_LIBARCHIVE
fs::path safe_name(const char *raw) {
    if (!raw)
        throw std::runtime_error("Missing archive path");
    std::string s = raw;
    std::replace(s.begin(), s.end(), '\\', '/');
    if (s.empty() || s.front() == '/' || s.find(':') != s.npos)
        throw std::runtime_error("Invalid archive path");
    while (s.ends_with('/'))
        s.pop_back();
    if (s.empty())
        throw std::runtime_error("Empty archive path");
    size_t start = 0;
    while (start < s.size()) {
        auto end = s.find('/', start);
        if (end == s.npos)
            end = s.size();
        auto part = s.substr(start, end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ')
            throw std::runtime_error("Unsafe archive path");
        for (unsigned char c : part)
            if (c < 32 || c == '<' || c == '>' || c == '\"' || c == '|' || c == '?' || c == '*')
                throw std::runtime_error("Unsafe archive path");
        auto base = lower(part.substr(0, part.find('.')));
        if (base == "con" || base == "prn" || base == "aux" || base == "nul" ||
            (base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '0' &&
             base[3] <= '9'))
            throw std::runtime_error("Reserved archive path");
        start = end + 1;
    }
    return fs::path(std::u8string(s.begin(), s.end()));
}
#endif
} // namespace
bool supported_archive(std::string_view name) {
    auto s = lower(std::string(name));
    return s.ends_with(".zip")
#ifdef BOTW_MOD_LIBARCHIVE
           || s.ends_with(".7z") || s.ends_with(".rar")
#endif
        ;
}
InstallResult install_archive(const fs::path &file, const fs::path &root, std::string_view name,
                              std::string_view pack_path) {
    if (lower(file.extension().string()) == ".zip")
        return install_zip(file, root, name, pack_path);
#ifndef BOTW_MOD_LIBARCHIVE
    throw std::runtime_error(
        "7z/RAR require a packaged archive backend; extract on a PC and import a folder");
#else
    if (!supported_archive(file.filename().string()))
        throw std::runtime_error("Unsupported archive");
    fs::create_directories(root / ".downloads");
    fs::path stage;
    for (unsigned i = 0; i < 10000; ++i) {
        auto p = root / ".downloads" / ("archive-" + std::to_string(i));
        if (fs::create_directory(p)) {
            stage = p;
            break;
        }
    }
    if (stage.empty())
        throw std::runtime_error("Cannot stage archive");
    fs::path dest;
    try {
        std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
        if (!reader)
            throw std::runtime_error("Archive allocation failed");
        archive_read_support_filter_all(reader.get());
        archive_read_support_format_7zip(reader.get());
        archive_read_support_format_rar(reader.get());
        archive_read_support_format_rar5(reader.get());
#ifdef _WIN32
        int opened = archive_read_open_filename_w(reader.get(), file.c_str(), 65536);
#else
        int opened = archive_read_open_filename(reader.get(), file.c_str(), 65536);
#endif
        if (opened != ARCHIVE_OK)
            throw std::runtime_error("Cannot open archive");
        archive_entry *entry = nullptr;
        std::set<std::string> names;
        uint64_t total = 0;
        unsigned count = 0;
        for (;;) {
            int status = archive_read_next_header(reader.get(), &entry);
            if (status == ARCHIVE_EOF)
                break;
            if (status != ARCHIVE_OK || ++count > 20000 || archive_entry_symlink(entry) ||
                archive_entry_hardlink(entry) || archive_entry_is_encrypted(entry) > 0)
                throw std::runtime_error("Unsafe archive entry");
            auto path = safe_name(archive_entry_pathname_utf8(entry));
            auto key = path.generic_u8string();
            if (!names.insert(lower(std::string(key.begin(), key.end()))).second)
                throw std::runtime_error("Duplicate archive path");
            auto type = archive_entry_filetype(entry);
            auto output = stage / path;
            if (type == AE_IFDIR) {
                fs::create_directories(output);
                continue;
            }
            auto size = archive_entry_size(entry);
            if (type != AE_IFREG || size < 0 || uint64_t(size) > uint64_t(4) * 1024 * 1024 * 1024 - total)
                throw std::runtime_error("Archive too large");
            total += uint64_t(size);
            fs::create_directories(output.parent_path());
            std::ofstream out(output, std::ios::binary);
            uint64_t written = 0;
            std::array<char, 65536> buffer;
            for (;;) {
                auto n = archive_read_data(reader.get(), buffer.data(), buffer.size());
                if (n == 0)
                    break;
                if (n < 0 || uint64_t(n) > uint64_t(size) - written)
                    throw std::runtime_error("Invalid archive data");
                out.write(buffer.data(), n);
                written += uint64_t(n);
                if (!out)
                    throw std::runtime_error("Archive write failed");
            }
            out.close();
            if (!out || written != uint64_t(size))
                throw std::runtime_error("Incomplete archive");
        }
        reader.reset();
        fs::path source;
        if (!pack_path.empty()) {
            for (auto &e : fs::recursive_directory_iterator(stage))
                if (e.is_directory() && selected_pack_directory(e.path(), pack_path) &&
                    fs::is_regular_file(e.path() / "rules.txt")) {
                    source = e.path();
                    break;
                }
        } else
            source = find_root(stage, 0, false);
        bool graphics = !pack_path.empty() || source.empty();
        if (graphics && source.empty() && pack_path.empty())
            source = find_root(stage, 0, true);
        if (source.empty())
            throw std::runtime_error("Archive must contain one selected mod");
        std::string clean;
        for (unsigned char c : name)
            if (clean.size() < 100)
                clean += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                          c == '_' || c == '-')
                             ? char(c)
                             : '_';
        if (clean.empty())
            clean = "mod";
        clean = "mod_" + clean;
        for (unsigned i = 0; i < 10000; ++i) {
            auto p = root / (clean + (i ? "_" + std::to_string(i) : ""));
            if (fs::create_directory(p)) {
                dest = p;
                break;
            }
        }
        if (dest.empty())
            throw std::runtime_error("Cannot create mod");
        for (auto &e : fs::directory_iterator(source))
            fs::rename(e.path(), dest / e.path().filename());
        if (graphics) {
            std::ofstream marker(dest / ".imported-graphics");
            marker << "Recompile to apply this graphic pack.\n";
            marker.close();
            if (!marker)
                throw std::runtime_error("Marker write failed");
        }
        fs::remove_all(stage);
        return {dest, graphics};
    } catch (...) {
        std::error_code ec;
        fs::remove_all(stage, ec);
        if (!dest.empty())
            fs::remove_all(dest, ec);
        throw;
    }
#endif
}
} // namespace mods
