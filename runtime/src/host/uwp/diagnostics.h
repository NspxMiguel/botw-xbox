// SPDX-License-Identifier: MIT
#pragma once
#include "formats/reader.h"
#include "setup.h"
#include "strings.h"
#include <cctype>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <tuple>
namespace uwp::setup {
enum class IssueKind { missing, wrong_size, version, title, incomplete, optional };
struct Issue {
    std::string path;
    IssueKind kind;
    uint64_t value = 0;
};
struct Diagnosis {
    std::string folder;
    Format format = Format::folder;
    std::vector<Issue> issues;
    std::string source;
    bool incomplete() const {
        return std::any_of(issues.begin(), issues.end(),
                           [](auto &i) { return i.kind != IssueKind::optional; });
    }
};
// Both filesystem fixtures and brokered USB storage use these exact checks.
struct Probe {
    std::function<std::optional<uint64_t>(const std::string &)> file;
    std::function<bool(const std::string &)> directory, nonempty;
    std::function<formats::Reader(const std::string &)> reader;
};
// Later layers replace files, matching the extraction merge order.
inline Probe layered_probe(std::vector<Probe> layers) {
    auto shared = std::make_shared<std::vector<Probe>>(std::move(layers));
    return {[shared](const std::string &path) -> std::optional<uint64_t> {
                for (auto i = shared->rbegin(); i != shared->rend(); ++i)
                    if (auto size = i->file(path))
                        return size;
                return {};
            },
            [shared](const std::string &path) {
                return std::any_of(shared->begin(), shared->end(),
                                   [&](auto &p) { return p.directory(path); });
            },
            [shared](const std::string &path) {
                return std::any_of(shared->begin(), shared->end(), [&](auto &p) { return p.nonempty(path); });
            },
            [shared](const std::string &path) {
                for (auto i = shared->rbegin(); i != shared->rend(); ++i)
                    if (i->file(path))
                        return i->reader(path);
                throw std::runtime_error("Missing layered file");
            }};
}
inline std::string hex_id(uint64_t value, unsigned width) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(int(width)) << value;
    return out.str();
}
inline bool botw_title(uint64_t id) {
    auto family = id >> 32;
    auto title = uint32_t(id);
    return (family == 0x00050000 || family == 0x0005000e || family == 0x0005000c) &&
           (title == 0x101c9300 || title == 0x101c9400 || title == 0x101c9500);
}
inline bool game_name(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (name.find("botw") != name.npos || name.find("zelda") != name.npos ||
        name.find("breath of the wild") != name.npos)
        return true;
    if (name.size() != 16 ||
        !std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isxdigit(c); }))
        return false;
    return botw_title(std::stoull(name, nullptr, 16));
}
inline bool looks_unpacked(const Probe &p, const std::string &name) {
    if (game_name(name))
        return true;
    for (auto path : {"code/app.xml", "meta/meta.xml"}) {
        try {
            if (!p.file(path))
                continue;
            auto r = p.reader(path);
            if (r.size > 1024 * 1024)
                continue;
            auto bytes = r.bytes(0, size_t(r.size));
            auto id = title_id(std::string(bytes.begin(), bytes.end()));
            if (id && botw_title(*id))
                return true;
        } catch (...) {
        }
    }
    return false;
}
// Discovery and the picker require a matching, complete diagnosis before offering a source.
inline bool usable_source(const std::vector<Diagnosis> &diagnoses, const std::string &folder, Format format,
                          const std::string &source = "") {
    return std::any_of(diagnoses.begin(), diagnoses.end(), [&](auto &d) {
        return d.folder == folder && d.format == format && d.source == source && !d.incomplete();
    });
}
// A scan repeats every second; remember what was logged so only new or changed verdicts reach the log.
struct LogMemo {
    std::map<std::string, std::string> last;
    std::string drives;
    // Forget everything when the set of drives changes, so the new layout is logged in full.
    void drive_list(const std::string &now) {
        if (now != drives) {
            last.clear();
            drives = now;
        }
    }
    bool changed(const std::string &key, const std::string &line) {
        auto it = last.find(key);
        if (it != last.end() && it->second == line)
            return false;
        last[key] = line;
        return true;
    }
};
// The same game can be reached by several paths (picked folder and drive, layered base and its layers).
struct ItemKey {
    std::string folder;
    Format format = Format::folder;
    std::string file;
    std::vector<std::string> layers;
};
inline std::string normal_path(std::string path) {
    for (auto &c : path)
        c = c == '/' ? '\\' : char(std::tolower((unsigned char)c));
    while (path.size() > 1 && path.back() == '\\')
        path.pop_back();
    return path;
}
// Indices of the first item per (folder, format, file); an item whose folder is a layer of another item is
// the same game as that layered item.
inline std::vector<size_t> distinct_items(const std::vector<ItemKey> &items) {
    std::vector<size_t> kept;
    std::set<std::tuple<std::string, Format, std::string>> seen;
    for (size_t i = 0; i < items.size(); ++i) {
        auto folder = normal_path(items[i].folder);
        bool layer = false;
        for (size_t j = 0; j < items.size() && !layer; ++j)
            if (j != i)
                for (auto &l : items[j].layers)
                    layer |= normal_path(l) == folder && folder != normal_path(items[j].folder) &&
                             items[j].format == items[i].format;
        if (!layer && seen.emplace(folder, items[i].format, items[i].file).second)
            kept.push_back(i);
    }
    return kept;
}
inline Diagnosis diagnose(const Probe &p, std::string folder, Format format, const std::string &name = "") {
    Diagnosis d{std::move(folder), format, {}, name};
    auto need = [&](const std::string &path) {
        if (!p.file(path))
            d.issues.push_back({path, IssueKind::missing});
    };
    auto bytes = [&](const std::string &path, size_t limit) {
        auto r = p.reader(path);
        formats::require(r.size <= limit, "Metadata too large");
        return r.bytes(0, size_t(r.size));
    };
    if (format == Format::folder) {
        for (auto path : {"code/U-King.rpx", "code/app.xml", "code/cos.xml", "meta/meta.xml"})
            need(path);
        if (!p.directory("content") || !p.nonempty("content"))
            d.issues.push_back({"content/", IssueKind::missing});
        need("content/Pack/Bootup.pack");
        if (p.file("code/app.xml")) {
            try {
                auto b = bytes("code/app.xml", 1024 * 1024);
                auto v = title_version(std::string(b.begin(), b.end()));
                if (!v || *v != 208)
                    d.issues.push_back({"code/U-King.rpx", IssueKind::version, v.value_or(0)});
            } catch (...) {
                d.issues.push_back({"code/app.xml", IssueKind::incomplete});
            }
        }
        if (p.file("meta/meta.xml")) {
            try {
                auto b = bytes("meta/meta.xml", 1024 * 1024);
                auto id = title_id(std::string(b.begin(), b.end()));
                if (!id)
                    d.issues.push_back({"meta/meta.xml", IssueKind::incomplete});
                else if (*id != 0x00050000101c9400ull && *id != 0x00050000101c9500ull &&
                         *id != 0x00050000101c9300ull)
                    d.issues.push_back({"meta/meta.xml", IssueKind::title, *id});
            } catch (...) {
                d.issues.push_back({"meta/meta.xml", IssueKind::incomplete});
            }
        }
        if (!p.directory("aoc"))
            d.issues.push_back({"aoc/", IssueKind::optional});
    } else {
        try {
            auto r = p.reader(name);
            if (format == Format::zip) {
                // Validate directory, local headers and payload bounds without inflating gigabytes.
                (void)formats::zip(r);
            } else if (format == Format::wua) {
                formats::require(r.size >= 144, "Truncated WUA");
                auto f = r.bytes(r.size - 144, 144);
                formats::require(formats::integer(f, 128, 8) == r.size &&
                                     formats::integer(f, 136, 4) == 0x61bf3a01 &&
                                     formats::integer(f, 140, 4) == 0x169f52d6,
                                 "Invalid WUA");
                for (size_t i = 0; i < 6; ++i) {
                    auto at = formats::integer(f, i * 16, 8), size = formats::integer(f, i * 16 + 8, 8);
                    formats::require(at <= r.size - 144 && size <= r.size - 144 - at,
                                     "Truncated WUA section");
                }
            } else {
                auto h = r.bytes(0, 32);
                const uint8_t magic[] = {0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c};
                formats::require(std::equal(std::begin(magic), std::end(magic), h.begin()), "Invalid 7z");
                auto at = formats::integer(h, 12, 8, true), size = formats::integer(h, 20, 8, true);
                formats::require(r.size >= 32 && at <= r.size - 32 && size <= r.size - 32 - at && size,
                                 "Truncated 7z");
                formats::require(crc32(0, h.data() + 12, 20) == formats::integer(h, 8, 4, true),
                                 "Invalid 7z header");
            }
        } catch (...) {
            d.issues.push_back({name, IssueKind::incomplete});
        }
    }
    return d;
}
// Recognition uses archive member paths, never the USB filename. Unreadable/opaque
// archives with no recognizable member names are silently ignored.
inline bool title_member(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    std::istringstream parts(path);
    std::string part;
    while (std::getline(parts, part, '/'))
        if (((part == "code" || part == "content" || part == "meta") && !parts.eof()) ||
            part.ends_with(".wua") || part == "u-king.rpx")
            return true;
    return false;
}
inline bool looks_like_title(const Probe &p, Format format, const std::string &name) {
    if (format == Format::wua)
        return true;
    bool found = false;
    try {
        auto r = p.reader(name);
        if (format == Format::zip) {
            (void)formats::zip(r, [&](auto &path) { found |= title_member(path); });
        } else if (format == Format::seven_zip) {
            auto h = r.bytes(0, 32);
            const uint8_t magic[] = {0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c};
            formats::require(std::equal(std::begin(magic), std::end(magic), h.begin()), "Invalid 7z");
            auto at = formats::integer(h, 12, 8, true), size = formats::integer(h, 20, 8, true);
            formats::require(size <= 8 * 1024 * 1024 && r.size >= 32 && at <= r.size - 32 &&
                                 size <= r.size - 32 - at,
                             "Invalid 7z directory");
            auto header = r.bytes(32 + at, size_t(size));
            // Encoded/encrypted headers are opaque here (.7z extraction already requires a PC).
            if (header.empty() || header[0] != 1)
                return false;
            auto number = [&](size_t &pos) {
                auto first = header.at(pos++);
                uint64_t value = 0;
                unsigned mask = 0x80;
                for (unsigned i = 0; i < 8; ++i, mask >>= 1) {
                    if (!(first & mask))
                        return value | (uint64_t(first & (mask - 1)) << (8 * i));
                    value |= uint64_t(header.at(pos++)) << (8 * i);
                }
                return value;
            };
            for (size_t start = 1; start < header.size(); ++start) {
                if (header[start] != 5)
                    continue; // FilesInfo
                try {
                    size_t pos = start + 1;
                    auto count = number(pos);
                    formats::require(count && count <= 200000, "Invalid 7z file count");
                    while (header.at(pos)) {
                        auto property = header.at(pos++);
                        auto length = number(pos);
                        formats::require(length <= header.size() - pos, "Invalid 7z property");
                        if (property == 0x11 && length && header[pos] == 0) {
                            std::string path;
                            uint64_t names = 0;
                            bool title = false;
                            for (size_t i = pos + 1; i + 1 < pos + length; i += 2) {
                                auto c = formats::integer(header, i, 2, true);
                                if (!c) {
                                    title |= title_member(path);
                                    path.clear();
                                    ++names;
                                } else
                                    path += c < 128 ? char(c) : '?';
                            }
                            if (names == count && path.empty())
                                found |= title;
                        }
                        pos += size_t(length);
                    }
                } catch (...) {
                }
            }
        }
    } catch (...) { /* A recognized ZIP with damaged payloads still needs a diagnosis. */
    }
    return found;
}
// One bounded scan session owns this cache; archive size and timestamps invalidate entries.
struct DiagnosticCache {
    struct Entry {
        uint64_t size;
        int64_t mtime;
        bool title;
        Diagnosis diagnosis;
        bool seen = false;
    };
    std::map<std::string, Entry> entries;
    void begin() {
        for (auto &[path, entry] : entries)
            entry.seen = false;
    }
    void end() {
        std::erase_if(entries, [](auto &pair) { return !pair.second.seen; });
    }
    std::optional<Diagnosis> get(const Probe &p, std::string folder, Format format, const std::string &name,
                                 uint64_t size, int64_t mtime) {
        auto path = folder + "/" + name;
        auto it = entries.find(path);
        if (it == entries.end() || it->second.size != size || it->second.mtime != mtime) {
            bool title = looks_like_title(p, format, name);
            it = entries
                     .insert_or_assign(path, Entry{size, mtime, title,
                                                   title ? diagnose(p, folder, format, name) : Diagnosis{}})
                     .first;
        }
        auto &entry = it->second;
        entry.seen = true;
        if (!entry.title)
            return {};
        return entry.diagnosis;
    }
};
inline std::wstring issue_text(const Issue &i, bool pt) {
    auto t = [pt](Text k) { return std::wstring(text(k, pt)); };
    if (i.kind == IssueKind::optional)
        return t(Text::dlc_optional);
    if (i.kind == IssueKind::version) {
        auto h = hex_id(i.value, 4);
        return t(Text::fix_version) + std::to_wstring(i.value) + L" (0x" + std::wstring(h.begin(), h.end()) +
               t(Text::fix_version_end);
    }
    if (i.kind == IssueKind::title) {
        auto h = hex_id(i.value, 16);
        return t(Text::fix_title) + std::wstring(h.begin(), h.end()) +
               L" — BotW: 00050000101c9400 / 101c9500 / 101c9300";
    }
    return t(i.kind == IssueKind::wrong_size   ? Text::fix_size
             : i.kind == IssueKind::incomplete ? Text::fix_incomplete
                                               : Text::fix_missing);
}
inline Probe filesystem_probe(std::filesystem::path root) {
    return {[root](const std::string &s) -> std::optional<uint64_t> {
                std::error_code ec;
                if (!std::filesystem::is_regular_file(root / s, ec))
                    return {};
                auto n = std::filesystem::file_size(root / s, ec);
                return ec ? std::nullopt : std::optional<uint64_t>(n);
            },
            [root](const std::string &s) {
                std::error_code ec;
                return std::filesystem::is_directory(root / s, ec);
            },
            [root](const std::string &s) {
                std::error_code ec;
                return !std::filesystem::is_empty(root / s, ec) && !ec;
            },
            [root](const std::string &s) {
                auto input = std::make_shared<std::ifstream>(root / s, std::ios::binary | std::ios::ate);
                formats::require(bool(*input), "Unreadable file");
                auto size = uint64_t(input->tellg());
                return formats::Reader{size, [input](uint64_t at, std::span<uint8_t> out) {
                                           input->clear();
                                           input->seekg(std::streamoff(at));
                                           input->read(reinterpret_cast<char *>(out.data()),
                                                       std::streamsize(out.size()));
                                           formats::require(bool(*input), "Short read");
                                       }};
            }};
}
inline std::vector<Diagnosis> diagnose_tree(const std::filesystem::path &root) {
    std::vector<Diagnosis> result;
    std::function<void(std::filesystem::path, unsigned)> visit = [&](auto dir, unsigned depth) {
        auto p = filesystem_probe(dir);
        bool unpacked = looks_unpacked(p, dir.filename().string());
        if (unpacked)
            result.push_back(diagnose(p, dir.generic_string(), Format::folder));
        std::error_code ec;
        for (auto &e : std::filesystem::directory_iterator(
                 dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (e.is_symlink(ec))
                continue;
            if (e.is_regular_file(ec)) {
                if (auto f = file_format(e.path().extension().string());
                    f && looks_like_title(p, *f, e.path().filename().string()))
                    result.push_back(diagnose(p, dir.generic_string(), *f, e.path().filename().string()));
            } else if (depth < kSearchDepth && e.is_directory(ec)) {
                auto name = e.path().filename().string();
                if (!unpacked || (name != "code" && name != "content" && name != "meta" && name != "aoc"))
                    visit(e.path(), depth + 1);
            }
        }
    };
    visit(root, 0);
    return result;
}
} // namespace uwp::setup
