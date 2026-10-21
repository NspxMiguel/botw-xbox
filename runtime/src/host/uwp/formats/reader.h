// SPDX-License-Identifier: MIT
#pragma once
#include "crypto.h"
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <zlib.h>
#include <zstd.h>
namespace uwp::formats {
inline void require(bool valid, const char *message) {
    if (!valid)
        throw std::runtime_error(message);
}
inline uint64_t integer(std::span<const uint8_t> bytes, size_t offset, size_t count, bool little = false) {
    require(offset <= bytes.size() && count <= bytes.size() - offset && count <= 8, "Truncated integer");
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i)
        value = (value << 8) | bytes[offset + (little ? count - 1 - i : i)];
    return value;
}
struct Reader {
    uint64_t size = 0;
    std::function<void(uint64_t, std::span<uint8_t>)> read;
    Reader() = default;
    Reader(uint64_t length, std::function<void(uint64_t, std::span<uint8_t>)> operation) : size(length) {
        auto shared = std::make_shared<decltype(operation)>(std::move(operation));
        read = [shared](uint64_t offset, std::span<uint8_t> out) { (*shared)(offset, out); };
    }
    Bytes bytes(uint64_t offset, size_t count) const {
        require(offset <= size && count <= size - offset, "Read outside image");
        Bytes result(count);
        read(offset, result);
        return result;
    }
};
inline Reader memory_reader(const Bytes &bytes) {
    return {bytes.size(), [&bytes](uint64_t offset, std::span<uint8_t> out) {
                require(offset <= bytes.size() && out.size() <= bytes.size() - offset, "Short memory read");
                std::copy_n(bytes.begin() + offset, out.size(), out.begin());
            }};
}
inline bool safe_path(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.front() == '\\' || path.size() > 4096)
        return false;
    while (!path.empty()) {
        auto end = path.find('/');
        auto part = path.substr(0, end);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ')
            return false;
        for (unsigned char c : part)
            if (c < 32 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
                c == '|')
                return false;
        auto stem = part.substr(0, part.find('.'));
        std::string upper(stem);
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return char(std::toupper(c)); });
        if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL" ||
            (upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) && upper[3] >= '1' &&
             upper[3] <= '9'))
            return false;
        if (end == path.npos)
            break;
        path.remove_prefix(end + 1);
    }
    return true;
}
using Sink = std::function<void(std::span<const uint8_t>)>;
struct Entry {
    std::string path;
    uint64_t size = 0;
    std::function<void(const Sink &)> extract;
};
inline void validate_entries(const std::vector<Entry> &entries) {
    require(entries.size() <= 200000, "Too many files");
    std::set<std::string> names;
    uint64_t total = 0;
    for (auto &entry : entries) {
        require(safe_path(entry.path), "Unsafe archive path");
        std::string folded = entry.path;
        std::transform(folded.begin(), folded.end(), folded.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        require(names.insert(folded).second, "Duplicate archive path");
        require(entry.size <= 128ull * 1024 * 1024 * 1024 &&
                    total <= 128ull * 1024 * 1024 * 1024 - entry.size,
                "Archive too large");
        total += entry.size;
    }
}
inline Reader disc_reader(Reader raw) {
    if (raw.size < 32)
        return raw;
    auto header = raw.bytes(0, 32);
    if (integer(header, 0, 4, true) != 0x30585557 || integer(header, 4, 4, true) != 0x1099d02e)
        return raw;
    uint64_t sector = integer(header, 8, 4, true), size = integer(header, 16, 8, true);
    require(sector >= 16 && sector <= 16 * 1024 * 1024 && (sector & (sector - 1)) == 0,
            "Invalid WUX sector size");
    require(size > 0 && size <= 128ull * 1024 * 1024 * 1024, "Invalid WUX image size");
    uint64_t count = (size + sector - 1) / sector;
    require(count <= 16 * 1024 * 1024, "WUX index too large");
    auto table = raw.bytes(32, size_t(count * 4));
    uint64_t base = (32 + count * 4 + sector - 1) / sector * sector;
    for (size_t i = 0; i < count; ++i) {
        uint64_t off = base + integer(table, i * 4, 4, true) * sector;
        require(off <= raw.size && sector <= raw.size - off, "Invalid WUX sector index");
    }
    return {
        size, [raw, table = std::move(table), sector, base, size](uint64_t offset, std::span<uint8_t> out) {
            require(offset <= size && out.size() <= size - offset, "Read outside WUX");
            while (!out.empty()) {
                size_t n = size_t(std::min<uint64_t>(out.size(), sector - offset % sector));
                auto chunk = raw.bytes(base + integer(table, size_t(offset / sector * 4), 4, true) * sector +
                                           offset % sector,
                                       n);
                std::copy(chunk.begin(), chunk.end(), out.begin());
                out = out.subspan(n);
                offset += n;
            }
        }};
}
inline Reader decrypted_cluster(Reader encrypted, Key key, uint16_t index, bool hashed, Bytes h3 = {}) {
    if (!hashed)
        return {encrypted.size, [encrypted, key, index](uint64_t offset, std::span<uint8_t> out) {
                    require(offset <= encrypted.size && out.size() <= encrypted.size - offset,
                            "Cluster overflow");
                    while (!out.empty()) {
                        uint64_t start = offset / 16 * 16;
                        size_t count = size_t(std::min<uint64_t>(out.size() + offset % 16, 1024 * 1024));
                        count = (count + 15) / 16 * 16;
                        Key iv{};
                        iv[0] = uint8_t(index >> 8);
                        iv[1] = uint8_t(index);
                        if (start) {
                            auto prev = encrypted.bytes(start - 16, 16);
                            std::copy(prev.begin(), prev.end(), iv.begin());
                        }
                        auto data = aes_cbc(key, iv, encrypted.bytes(start, count));
                        size_t n = std::min(out.size(), data.size() - size_t(offset - start));
                        std::copy_n(data.begin() + (offset - start), n, out.begin());
                        out = out.subspan(n);
                        offset += n;
                    }
                }};
    require(encrypted.size % 0x10000 == 0, "Truncated hashed content");
    return {
        encrypted.size / 0x10000 * 0xfc00,
        [encrypted, key, h3 = std::move(h3)](uint64_t offset, std::span<uint8_t> out) {
            while (!out.empty()) {
                uint64_t block = offset / 0xfc00;
                auto raw = encrypted.bytes(block * 0x10000, 0x10000);
                auto hashes = aes_cbc(key, {}, std::span(raw).first(0x400));
                Key iv{};
                std::copy_n(hashes.begin() + (block % 16) * 20, 16, iv.begin());
                auto data = aes_cbc(key, iv, std::span(raw).subspan(0x400));
                if (!h3.empty()) {
                    auto h1 = sha1(std::span(hashes).first(0x140));
                    auto h2 = sha1(std::span(hashes).subspan(0x140, 0x140));
                    auto top = sha1(std::span(hashes).subspan(0x280, 0x140));
                    require(std::equal(h1.begin(), h1.end(), hashes.begin() + 0x140 + (block / 16 % 16) * 20),
                            "Content H1 mismatch");
                    require(
                        std::equal(h2.begin(), h2.end(), hashes.begin() + 0x280 + (block / 256 % 16) * 20),
                        "Content H2 mismatch");
                    require(block / 4096 * 20 <= h3.size() && 20 <= h3.size() - size_t(block / 4096 * 20),
                            "Truncated .h3");
                    require(std::equal(top.begin(), top.end(), h3.begin() + block / 4096 * 20),
                            "Content H3 mismatch");
                }
                auto digest = sha1(data);
                require(std::equal(digest.begin(), digest.end(), hashes.begin() + (block % 16) * 20),
                        "Content H0 mismatch");
                size_t within = size_t(offset % 0xfc00), n = std::min(out.size(), data.size() - within);
                std::copy_n(data.begin() + within, n, out.begin());
                out = out.subspan(n);
                offset += n;
            }
        }};
}
inline void stream_range(const Reader &reader, uint64_t offset, uint64_t size, const Sink &sink) {
    while (size) {
        size_t n = size_t(std::min<uint64_t>(size, 1024 * 1024));
        auto chunk = reader.bytes(offset, n);
        sink(chunk);
        offset += n;
        size -= n;
    }
}
struct Fst {
    struct Cluster {
        uint64_t sector, size;
        uint8_t mode;
    };
    std::vector<Cluster> clusters;
    struct File {
        std::string path;
        uint64_t offset, size;
        uint16_t cluster;
    };
    std::vector<File> files;
    explicit Fst(std::span<const uint8_t> bytes) {
        require(integer(bytes, 0, 4) == 0x46535400, "Invalid FST (wrong key?)");
        uint64_t factor = integer(bytes, 4, 4), count = integer(bytes, 8, 4);
        require(factor && factor <= 0x10000 && count <= 65536, "Invalid FST clusters");
        for (size_t i = 0; i < count; ++i) {
            size_t p = 0x20 + i * 0x20;
            clusters.push_back(
                {integer(bytes, p, 4), integer(bytes, p + 4, 4), uint8_t(integer(bytes, p + 0x14, 1))});
        }
        size_t tree = 0x20 + size_t(count) * 0x20;
        size_t entries = size_t(integer(bytes, tree + 8, 4));
        require(entries > 0 && entries <= 200000 && tree <= bytes.size() &&
                    entries <= (bytes.size() - tree) / 16,
                "Invalid FST entries");
        size_t names = tree + entries * 16;
        struct Parent {
            size_t end;
            std::string path;
        };
        std::vector<Parent> parents{{entries, ""}};
        for (size_t i = 1; i < entries; ++i) {
            while (!parents.empty() && i >= parents.back().end)
                parents.pop_back();
            require(!parents.empty(), "Invalid FST hierarchy");
            size_t p = tree + i * 16;
            uint64_t type = integer(bytes, p, 4);
            size_t name = names + size_t(type & 0xffffff), end = name;
            require(name < bytes.size(), "Invalid FST name");
            while (end < bytes.size() && bytes[end])
                ++end;
            require(end < bytes.size(), "Unterminated FST name");
            std::string component(reinterpret_cast<const char *>(bytes.data() + name), end - name);
            require(component.find('/') == component.npos && safe_path(component), "Unsafe FST name");
            auto path = parents.back().path + component;
            uint64_t offset = integer(bytes, p + 4, 4), size = integer(bytes, p + 8, 4);
            if ((type >> 24) & 1) {
                require(size > i && size <= parents.back().end, "Invalid FST directory");
                parents.push_back({size, path + "/"});
            } else {
                uint16_t cluster = uint16_t(integer(bytes, p + 14, 2));
                require(cluster < clusters.size(), "Invalid FST cluster");
                files.push_back({path, offset * factor, size, cluster});
            }
        }
    }
    std::vector<Entry> entries(const std::function<Reader(uint16_t)> &cluster_reader) const {
        std::vector<Entry> result;
        for (auto &file : files) {
            auto reader = cluster_reader(file.cluster);
            require(file.offset <= reader.size && file.size <= reader.size - file.offset,
                    "FST file outside cluster");
            result.push_back(
                {file.path, file.size, [reader, offset = file.offset, size = file.size](const Sink &sink) {
                     stream_range(reader, offset, size, sink);
                 }});
        }
        validate_entries(result);
        return result;
    }
};
inline std::vector<Entry> wud(Reader raw, Key disc, Key common) {
    auto image = disc_reader(std::move(raw));
    require(integer(image.bytes(0x10000, 4), 0, 4) == 0xcc549eb9, "Invalid WUD header");
    auto table = aes_cbc(disc, {}, image.bytes(0x18000, 0x8000));
    require(integer(table, 0, 4) == 0xcca6e67b, "Wrong disc key");
    size_t count = size_t(integer(table, 0x1c, 4));
    require(count <= 240, "Invalid partition count");
    uint64_t si = 0, gm = 0;
    size_t gm_index = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t p = 0x800 + i * 0x80;
        auto base = integer(table, p + 0x20, 4) * 0x8000;
        if (table[p] == 'S' && table[p + 1] == 'I')
            si = base;
        if (table[p] == 'G' && table[p + 1] == 'M' && !gm) {
            gm = base;
            gm_index = i;
        }
    }
    require(si && gm, "Missing SI/GM partition");
    auto volume = [image](uint64_t base, Key key) {
        auto header = image.bytes(base, 0x40);
        require(integer(header, 0, 4) == 0xcc93a4f5, "Invalid partition header");
        size_t size = size_t(integer(header, 0x14, 4));
        uint64_t offset = integer(header, 0x18, 4) * 0x8000;
        require(size > 0 && size <= 64 * 1024 * 1024, "Invalid FST size");
        auto plain = aes_cbc(key, {}, image.bytes(base + offset, (size + 15) / 16 * 16));
        plain.resize(size);
        Fst fst(plain);
        return fst.entries([image, base, key, clusters = fst.clusters](uint16_t index) {
            auto cluster = clusters.at(index);
            uint64_t offset = base + cluster.sector * 0x8000, size = cluster.size * 0x8000;
            require(offset <= image.size && size <= image.size - offset, "Cluster outside disc");
            require(cluster.mode <= 2, "Unsupported cluster hash mode");
            Reader raw{size, [image, offset](uint64_t pos, std::span<uint8_t> out) {
                           image.read(offset + pos, out);
                       }};
            return decrypted_cluster(raw, key, index, cluster.mode == 2);
        });
    };
    auto system = volume(si, disc);
    char hex[] = "0123456789abcdef";
    std::string ticket_path;
    ticket_path += hex[(gm_index >> 4) & 15];
    ticket_path += hex[gm_index & 15];
    ticket_path += "/title.tik";
    Bytes ticket;
    for (auto &file : system)
        if (file.path == ticket_path) {
            require(file.size < 1024 * 1024, "Ticket too large");
            file.extract([&](auto data) { ticket.insert(ticket.end(), data.begin(), data.end()); });
        }
    return volume(gm, ticket_key(ticket, common));
}
// NUS content records are SHA-1 signed by the title metadata; encrypted bytes are never persisted internally.
inline std::vector<Entry> nus(const Bytes &tmd, const Bytes &ticket, Key common,
                              const std::function<Reader(const std::string &)> &open) {
    auto key = ticket_key(ticket, common);
    require(tmd.size() >= 0xb04, "Truncated title.tmd");
    size_t count = size_t(integer(tmd, 0x1de, 2));
    require(count > 0 && count <= 65535 && count <= (tmd.size() - 0xb04) / 48, "Invalid NUS content table");
    struct Content {
        uint32_t id;
        uint16_t index, type;
        uint64_t size;
    };
    std::vector<Content> contents;
    for (size_t i = 0; i < count; ++i) {
        size_t p = 0xb04 + i * 48;
        contents.push_back({uint32_t(integer(tmd, p, 4)), uint16_t(integer(tmd, p + 4, 2)),
                            uint16_t(integer(tmd, p + 6, 2)), integer(tmd, p + 8, 8)});
    }
    std::map<uint16_t, Reader> readers;
    for (auto content : contents) {
        char name[16];
        std::snprintf(name, sizeof(name), "%08x.app", content.id);
        auto encrypted = open(name);
        bool hashed = (content.type & 2) != 0;
        Bytes h3;
        if (hashed) {
            std::snprintf(name, sizeof(name), "%08x.h3", content.id);
            auto hashes = open(name);
            require(hashes.size > 0 && hashes.size <= 8 * 1024 * 1024, "Invalid .h3 size");
            h3 = hashes.bytes(0, size_t(hashes.size));
        }
        auto plain = decrypted_cluster(encrypted, key, content.index, hashed, std::move(h3));
        require(content.size <= encrypted.size, "Truncated NUS content");
        readers.emplace(content.index, std::move(plain));
    }
    require(readers.contains(0), "Missing NUS FST content");
    auto &fst_reader = readers.at(0);
    require(fst_reader.size <= 64 * 1024 * 1024, "NUS FST too large");
    Fst fst(fst_reader.bytes(0, size_t(fst_reader.size)));
    return fst.entries([&](uint16_t index) {
        require(readers.contains(index), "Missing .app content");
        return readers.at(index);
    });
}
// ZArchive v1 layout: https://github.com/Exzap/ZArchive (MIT-0).
// Footer and 16-entry compression offset records are always big endian.
inline std::vector<Entry> wua(Reader image, const std::function<void(uint64_t, uint64_t)> &progress = {}) {
    require(image.size >= 144, "Truncated WUA footer");
    auto footer = image.bytes(image.size - 144, 144);
    require(integer(footer, 140, 4) == 0x169f52d6 && integer(footer, 136, 4) == 0x61bf3a01 &&
                integer(footer, 128, 8) == image.size,
            "Invalid WUA footer");
    packs::Sha256 integrity;
    uint64_t hashed = 0;
    stream_range(image, 0, image.size - 144, [&](auto data) {
        integrity.update(data.data(), data.size());
        hashed += data.size();
        if (progress)
            progress(hashed, image.size);
    });
    auto zero_footer = footer;
    std::fill(zero_footer.begin() + 96, zero_footer.begin() + 128, 0);
    integrity.update(zero_footer.data(), zero_footer.size());
    if (progress)
        progress(image.size, image.size);
    std::string expected;
    for (size_t i = 96; i < 128; ++i) {
        expected += "0123456789abcdef"[footer[i] >> 4];
        expected += "0123456789abcdef"[footer[i] & 15];
    }
    require(integrity.finish() == expected, "WUA integrity hash mismatch");
    std::array<uint64_t, 6> offsets{}, sizes{};
    for (size_t i = 0; i < 6; ++i) {
        offsets[i] = integer(footer, i * 16, 8);
        sizes[i] = integer(footer, i * 16 + 8, 8);
        require(offsets[i] <= image.size - 144 && sizes[i] <= image.size - 144 - offsets[i],
                "Invalid WUA section");
    }
    require(sizes[1] % 40 == 0 && sizes[1] > 0 && sizes[1] <= 64 * 1024 * 1024 &&
                sizes[2] <= 32 * 1024 * 1024 && sizes[3] % 16 == 0 && sizes[3] >= 16 &&
                sizes[3] <= 32 * 1024 * 1024,
            "Invalid WUA tables");
    auto records = image.bytes(offsets[1], size_t(sizes[1])),
         names = image.bytes(offsets[2], size_t(sizes[2])), tree = image.bytes(offsets[3], size_t(sizes[3]));
    Reader plain{sizes[1] / 40 * 16 * 65536, [image, records, base = offsets[0], compressed_size = sizes[0]](
                                                 uint64_t pos, std::span<uint8_t> out) {
                     while (!out.empty()) {
                         uint64_t block = pos / 65536;
                         size_t record = size_t(block / 16 * 40), index = size_t(block % 16);
                         uint64_t offset = integer(records, record, 8);
                         for (size_t j = 0; j < index; ++j)
                             offset += integer(records, record + 8 + j * 2, 2) + 1;
                         size_t size = size_t(integer(records, record + 8 + index * 2, 2)) + 1;
                         require(offset <= compressed_size && size <= compressed_size - offset,
                                 "WUA block outside data");
                         auto packed = image.bytes(base + offset, size);
                         Bytes data(65536);
                         if (size == 65536)
                             data = std::move(packed);
                         else {
                             auto decoded =
                                 ZSTD_decompress(data.data(), data.size(), packed.data(), packed.size());
                             require(!ZSTD_isError(decoded) && decoded == 65536, "Invalid WUA zstd block");
                         }
                         size_t within = size_t(pos % 65536), n = std::min(out.size(), 65536 - within);
                         std::copy_n(data.begin() + within, n, out.begin());
                         pos += n;
                         out = out.subspan(n);
                     }
                 }};
    std::vector<Entry> result;
    std::set<size_t> visited;
    std::function<void(size_t, std::string, unsigned)> walk = [&](size_t node, std::string path,
                                                                  unsigned depth) {
        require(depth < 64 && node < tree.size() / 16 && visited.insert(node).second,
                "Invalid WUA hierarchy");
        size_t p = node * 16;
        uint64_t type = integer(tree, p, 4), name_offset = type & 0x7fffffff;
        std::string component;
        if (node) {
            require(name_offset < names.size(), "Invalid WUA name");
            size_t p_name = size_t(name_offset), length = names[p_name] & 0x7f;
            bool extended = (names[p_name] & 0x80) != 0;
            ++p_name;
            if (extended) {
                require(p_name < names.size(), "Truncated WUA name length");
                length |= size_t(names[p_name++]) << 7;
            }
            require(p_name <= names.size() && length <= names.size() - p_name, "Invalid WUA name length");
            component.assign(reinterpret_cast<const char *>(names.data() + p_name), length);
        } else
            require(type == 0x7fffffff || type == 0, "Invalid WUA root");
        if (node) {
            require(component.find('/') == component.npos && safe_path(component), "Unsafe WUA name");
            path += component;
        }
        if (type & 0x80000000) {
            uint64_t high = integer(tree, p + 12, 4),
                     offset = integer(tree, p + 4, 4) | ((high & 65535) << 32),
                     size = integer(tree, p + 8, 4) | ((high >> 16) << 32);
            require(offset <= plain.size && size <= plain.size - offset, "Invalid WUA file range");
            result.push_back({path, size, [plain, offset, size](const Sink &sink) {
                                  stream_range(plain, offset, size, sink);
                              }});
        } else {
            size_t first = size_t(integer(tree, p + 4, 4)), count = size_t(integer(tree, p + 8, 4));
            require(first <= tree.size() / 16 && count <= tree.size() / 16 - first,
                    "Invalid WUA directory range");
            if (node)
                path += '/';
            for (size_t i = 0; i < count; ++i)
                walk(first + i, path, depth + 1);
        }
    };
    walk(0, "", 0);
    validate_entries(result);
    return result;
}
inline std::vector<Entry> zip(Reader image, const std::function<void(const std::string &)> &visit = {}) {
    require(image.size >= 22, "Truncated ZIP");
    size_t tail_size = size_t(std::min<uint64_t>(image.size, 65557));
    auto tail = image.bytes(image.size - tail_size, tail_size);
    size_t eocd = tail.size();
    for (size_t i = tail.size() - 22;; --i) {
        if (integer(tail, i, 4, true) == 0x06054b50 &&
            i + 22 + integer(tail, i + 20, 2, true) == tail.size()) {
            eocd = i;
            break;
        }
        if (i == 0)
            break;
    }
    require(eocd < tail.size(), "Missing ZIP directory");
    require(integer(tail, eocd + 4, 4, true) == 0, "Multi-disk ZIP unsupported");
    size_t count = size_t(integer(tail, eocd + 10, 2, true));
    uint64_t directory = integer(tail, eocd + 16, 4, true);
    require(count < 65535 && directory != 0xffffffff, "ZIP64 unsupported");
    std::vector<Entry> result;
    for (size_t i = 0; i < count; ++i) {
        auto header = image.bytes(directory, 46);
        require(integer(header, 0, 4, true) == 0x02014b50, "Invalid ZIP directory");
        uint64_t flags = integer(header, 8, 2, true), method = integer(header, 10, 2, true),
                 crc = integer(header, 16, 4, true), packed = integer(header, 20, 4, true),
                 size = integer(header, 24, 4, true), offset = integer(header, 42, 4, true);
        size_t name_length = size_t(integer(header, 28, 2, true));
        auto name_bytes = image.bytes(directory + 46, name_length);
        std::string name(name_bytes.begin(), name_bytes.end());
        if (visit)
            visit(name);
        directory += 46 + name_length + integer(header, 30, 2, true) + integer(header, 32, 2, true);
        require(!(flags & 1) && (method == 0 || method == 8) && packed != 0xffffffff && size != 0xffffffff,
                "Encrypted/unsupported ZIP");
        require((integer(header, 38, 4, true) >> 16 & 0170000) != 0120000, "ZIP symlink rejected");
        if (name.ends_with('/')) {
            name.pop_back();
            require(safe_path(name), "Unsafe ZIP directory");
            continue;
        }
        auto local = image.bytes(offset, 30);
        require(integer(local, 0, 4, true) == 0x04034b50 && integer(local, 8, 2, true) == method,
                "Invalid ZIP local header");
        uint64_t data = offset + 30 + integer(local, 26, 2, true) + integer(local, 28, 2, true);
        require(data <= image.size && packed <= image.size - data, "Truncated ZIP payload");
        result.push_back(
            {name, size, [image, data, packed, size, method, crc](const Sink &sink) {
                 uint64_t remaining = packed, position = data, produced = 0;
                 uLong checksum = crc32(0, nullptr, 0);
                 z_stream stream{};
                 if (method == 8)
                     require(inflateInit2(&stream, -MAX_WBITS) == Z_OK, "Deflate init failed");
                 struct End {
                     z_stream &s;
                     bool active;
                     ~End() {
                         if (active)
                             inflateEnd(&s);
                     }
                 } end{stream, method == 8};
                 Bytes output(65536);
                 int status = Z_OK;
                 while (remaining) {
                     auto input = image.bytes(position, size_t(std::min<uint64_t>(remaining, 65536)));
                     position += input.size();
                     remaining -= input.size();
                     if (method == 0) {
                         produced += input.size();
                         require(produced <= size, "ZIP size overflow");
                         checksum = crc32(checksum, input.data(), uInt(input.size()));
                         sink(input);
                         continue;
                     }
                     require(status != Z_STREAM_END, "Trailing ZIP deflate data");
                     stream.next_in = input.data();
                     stream.avail_in = uInt(input.size());
                     do {
                         stream.next_out = output.data();
                         stream.avail_out = uInt(output.size());
                         status = inflate(&stream, Z_NO_FLUSH);
                         require(status == Z_OK || status == Z_STREAM_END, "Invalid deflate stream");
                         size_t n = output.size() - stream.avail_out;
                         produced += n;
                         require(produced <= size, "ZIP size overflow");
                         checksum = crc32(checksum, output.data(), uInt(n));
                         sink(std::span(output).first(n));
                         if (status == Z_STREAM_END) {
                             require(!stream.avail_in && !remaining, "Trailing ZIP data");
                             break;
                         }
                     } while (stream.avail_in || !stream.avail_out);
                 }
                 require(produced == size && checksum == crc && (method == 0 || status == Z_STREAM_END),
                         "ZIP size/CRC mismatch");
             }});
    }
    validate_entries(result);
    return result;
}
} // namespace uwp::formats
