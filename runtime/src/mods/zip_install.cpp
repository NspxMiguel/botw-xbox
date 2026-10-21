// SPDX-License-Identifier: MIT
#include <cstring>
#include <cctype>
#include "zip_install.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <stdexcept>
#include <vector>
#include <zlib.h>
namespace mods {
namespace {
namespace fs = std::filesystem;
[[noreturn]] void bad() { throw std::runtime_error("Invalid or unsupported ZIP archive"); }
uint16_t u16(const unsigned char *p) { return p[0] | uint16_t(p[1]) << 8; }
uint32_t u32(const unsigned char *p) { return u16(p) | uint32_t(u16(p + 2)) << 16; }
std::string lower(std::string s) {
    for (auto &c : s)
        if (c >= 'A' && c <= 'Z')
            c += 32;
    return s;
}
fs::path path8(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }
struct Archive {
    std::ifstream in;
    uint64_t size;
    explicit Archive(const fs::path &p) : in(p, std::ios::binary) {
        if (!in)
            bad();
        in.seekg(0, std::ios::end);
        size = static_cast<uint64_t>(in.tellg());
    }
    std::vector<unsigned char> read(uint64_t at, size_t n) {
        if (at > size || n > size - at)
            bad();
        std::vector<unsigned char> b(n);
        in.seekg(static_cast<std::streamoff>(at));
        if (n && !in.read(reinterpret_cast<char *>(b.data()), n))
            bad();
        return b;
    }
};
struct Entry {
    std::string name;
    uint32_t packed, size, crc, offset;
    uint16_t method, flags;
    bool directory;
};
std::string safe_path(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    if (s.empty() || s.front() == '/' || s.find_first_of(":\0", 0, 2) != s.npos)
        bad();
    size_t pos = 0;
    while (pos < s.size()) {
        auto end = s.find('/', pos);
        if (end == s.npos)
            end = s.size();
        auto part = s.substr(pos, end - pos);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ')
            bad();
        for (unsigned char c : part)
            if (c < 32 || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*')
                bad();
        auto base = lower(part.substr(0, part.find('.')));
        if (base == "con" || base == "prn" || base == "aux" || base == "nul" ||
            (base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '0' &&
             base[3] <= '9'))
            bad();
        pos = end + 1;
    }
    return s;
}
std::vector<Entry> entries(Archive &a) {
    if (a.size < 22)
        bad();
    auto tail = a.read(a.size - std::min<uint64_t>(a.size, 65557), std::min<uint64_t>(a.size, 65557));
    size_t e = tail.size() - 22;
    while (true) {
        if (u32(tail.data() + e) == 0x06054b50 && e + 22 + u16(tail.data() + e + 20) == tail.size())
            break;
        if (e == 0)
            bad();
        --e;
    }
    auto p = tail.data() + e;
    if (u16(p + 4) || u16(p + 6) || u16(p + 8) != u16(p + 10) || u16(p + 10) > 20000)
        bad();
    uint64_t at = u32(p + 16), end = at + u32(p + 12), total = 0;
    if (end > a.size - tail.size() + e)
        bad();
    std::vector<Entry> out;
    std::set<std::string> names;
    for (unsigned i = 0; i < u16(p + 10); ++i) {
        if (at + 46 > end)
            bad();
        auto h = a.read(at, 46);
        auto b = h.data();
        if (u32(b) != 0x02014b50)
            bad();
        uint16_t len = u16(b + 28), extra = u16(b + 30), comment = u16(b + 32), flags = u16(b + 8),
                 method = u16(b + 10);
        uint32_t attrs = u32(b + 38);
        unsigned mode = (attrs >> 16) & 0170000;
        if ((flags & ~uint16_t(0x80e)) || (method != 0 && method != 8) || mode == 0120000 ||
            (mode && mode != 0100000 && mode != 0040000) || u16(b + 34))
            bad();
        if (at + 46 + len + extra + comment > end)
            bad();
        auto raw = a.read(at + 46, len);
        std::string name = safe_path(std::string(raw.begin(), raw.end()));
        if (!names.insert(lower(name)).second)
            bad();
        Entry v{name, u32(b + 20), u32(b + 24), u32(b + 16), u32(b + 42), method, flags, name.back() == '/'};
        total += v.size;
        if (total > uint64_t(4) * 1024 * 1024 * 1024 || v.size == 0xffffffff || v.packed == 0xffffffff ||
            v.offset == 0xffffffff || (v.directory && v.size))
            bad();
        out.push_back(v);
        at += 46 + len + extra + comment;
    }
    if (at != end)
        bad();
    return out;
}
void extract(Archive &a, const Entry &e, const fs::path &root) {
    auto h = a.read(e.offset, 30);
    auto p = h.data();
    if (u32(p) != 0x04034b50 || u16(p + 6) != e.flags || u16(p + 8) != e.method)
        bad();
    auto raw = a.read(uint64_t(e.offset) + 30, u16(p + 26));
    if (safe_path(std::string(raw.begin(), raw.end())) != e.name)
        bad();
    uint64_t at = uint64_t(e.offset) + 30 + u16(p + 26) + u16(p + 28);
    if (at > a.size || e.packed > a.size - at)
        bad();
    auto dest = root / path8(e.name);
    if (e.directory) {
        fs::create_directories(dest);
        return;
    }
    fs::create_directories(dest.parent_path());
    std::ofstream out(dest, std::ios::binary);
    if (!out)
        bad();
    uint64_t written = 0;
    uLong crc = crc32(0, nullptr, 0);
    auto emit = [&](const unsigned char *data, size_t n) {
        if (written + n > e.size)
            bad();
        written += n;
        crc = crc32(crc, data, static_cast<uInt>(n));
        out.write(reinterpret_cast<const char *>(data), n);
        if (!out)
            bad();
    };
    if (e.method == 0) {
        if (e.packed != e.size)
            bad();
        for (uint32_t offset = 0; offset < e.packed;) {
            auto b = a.read(at + offset, std::min<uint32_t>(65536, e.packed - offset));
            emit(b.data(), b.size());
            offset += static_cast<uint32_t>(b.size());
        }
    } else {
        z_stream z{};
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
            bad();
        struct End {
            z_stream &z;
            ~End() { inflateEnd(&z); }
        } guard{z};
        std::array<unsigned char, 65536> output{};
        uint32_t consumed = 0;
        int status = Z_OK;
        while (consumed < e.packed && status != Z_STREAM_END) {
            auto b = a.read(at + consumed, std::min<uint32_t>(65536, e.packed - consumed));
            consumed += static_cast<uint32_t>(b.size());
            z.next_in = b.data();
            z.avail_in = static_cast<uInt>(b.size());
            do {
                z.next_out = output.data();
                z.avail_out = output.size();
                status = inflate(&z, Z_NO_FLUSH);
                if (status != Z_OK && status != Z_STREAM_END)
                    bad();
                emit(output.data(), output.size() - z.avail_out);
            } while (status != Z_STREAM_END && (z.avail_in || z.avail_out == 0));
        }
        if (status != Z_STREAM_END || z.total_in != e.packed)
            bad();
    }
    if (written != e.size || crc != e.crc)
        bad();
    out.close();
    if (!out)
        bad();
}
fs::path unique_dir(const fs::path &root, const std::string &name) {
    for (unsigned i = 0; i < 100000; ++i) {
        auto p = root / path8(name + (i ? " (" + std::to_string(i + 1) + ")" : ""));
        std::error_code error;
        if (fs::create_directory(p, error))
            return p;
        if (error && error != std::errc::file_exists)
            throw fs::filesystem_error("Create unique mod directory", p, error);
    }
    bad();
}
} // namespace
fs::path find_root(const fs::path &root, int depth, bool graphics) {
    if (graphics ? fs::is_regular_file(root / "rules.txt")
                 : fs::is_directory(root / "content") || fs::is_directory(root / "aoc"))
        return root;
    if (depth == 3)
        return {};
    std::vector<fs::path> dirs;
    for (auto &e : fs::directory_iterator(root))
        if (e.is_directory())
            dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    for (auto &p : dirs) {
        auto found = find_root(p, depth + 1, graphics);
        if (!found.empty())
            return found;
    }
    return {};
}
bool selected_pack_directory(const fs::path &directory, std::string_view selection) {
    auto modern = selection.find("BreathOfTheWild/");
    if (modern != selection.npos) {
        auto suffix = std::string(selection.substr(modern));
        auto candidate = directory.generic_u8string();
        std::string path(candidate.begin(), candidate.end());
        return path == suffix || (path.size() > suffix.size() && path.ends_with("/" + suffix));
    }
    auto name = fs::path(std::string(selection)).filename();
    return name.string().starts_with("BreathOfTheWild_") && directory.filename() == name;
}
InstallResult install_zip(const fs::path &archive, const fs::path &root, std::string_view name,
                          std::string_view pack_path) {
    Archive a(archive);
    auto list = entries(a);
    fs::create_directories(root / ".downloads");
    auto staging = unique_dir(root / ".downloads", "extract");
    fs::path destination;
    try {
        for (auto &e : list)
            extract(a, e, staging);
        fs::path source;
        if (!pack_path.empty()) {
            // Match an exact BotW pack directory inside a release, never the first pack in the ZIP.
            for (auto &entry : fs::recursive_directory_iterator(staging))
                if (entry.is_directory() && selected_pack_directory(entry.path(), pack_path) &&
                    fs::is_regular_file(entry.path() / "rules.txt")) {
                    source = entry.path();
                    break;
                }
            if (source.empty())
                bad();
        } else
            source = find_root(staging, 0, false);
        bool graphics = !pack_path.empty() || source.empty();
        if (graphics && source.empty())
            source = find_root(staging, 0, true);
        if (source.empty())
            bad();
        std::string clean;
        for (unsigned char c : name) {
            if (clean.size() >= 100)
                break;
            clean += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '_')
                         ? char(c)
                         : '_';
        }
        if (clean.empty())
            clean = "mod";
        static const char *reserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
                                         "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
                                         "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
        for (auto r : reserved)
            if (std::equal(clean.begin(), clean.end(), r, r + std::strlen(r),
                           [](char a, char b) { return std::toupper(static_cast<unsigned char>(a)) == b; }))
                clean.insert(0, "_");
        destination = unique_dir(root, clean);
        for (auto &e : fs::directory_iterator(source))
            fs::rename(e.path(), destination / e.path().filename());
        if (graphics) {
            std::ofstream marker(destination / ".imported-graphics");
            marker << "Recompile to apply this graphic pack.\n";
            marker.close();
            if (!marker)
                bad();
        }
        fs::remove_all(staging);
        return {destination, graphics};
    } catch (...) {
        std::error_code ec;
        fs::remove_all(staging, ec);
        if (!destination.empty())
            fs::remove_all(destination, ec);
        throw;
    }
}
} // namespace mods
