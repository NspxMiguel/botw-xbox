#include "loaded_rpx.h"
#include "../mem.h"
#include "../platform/host_file.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace packs {
namespace {
uint32_t be32(const uint8_t *p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 |
         p[3];
}
void sections(const std::vector<uint8_t> &raw, LoadedRpx &snapshot) {
  if (raw.size() < 52 || be32(raw.data()) != 0x7f454c46)
    return;
  const uint32_t offset = be32(raw.data() + 32);
  const unsigned stride = unsigned(raw[46]) << 8 | raw[47];
  const unsigned count = unsigned(raw[48]) << 8 | raw[49];
  if (stride < 40 || uint64_t(offset) + uint64_t(stride) * count > raw.size())
    return;
  for (unsigned i = 0; i < count; ++i) {
    const auto *s = raw.data() + offset + i * stride;
    const auto type = be32(s + 4), flags = be32(s + 8), address = be32(s + 12);
    uint32_t size = be32(s + 20);
    // Executable sections are writable in our data-only guest mapping: compiled
    // code never executes there, but guest code reads and pack constants do.
    if (!(flags & 2) || type == 0x80000002)
      continue;
    if (flags & 0x08000000) {
      uint32_t source = be32(s + 16);
      if (uint64_t(source) + 4 > raw.size())
        continue;
      size = be32(raw.data() + source);
    }
    if (size && uint64_t(address) + size <= 0x100000000ull) {
      snapshot.allocated.push_back({address, uint64_t(address) + size});
      if (flags & (1 | 4))
        snapshot.writable.push_back({address, uint64_t(address) + size});
    }
  }
}
} // namespace

bool LoadedRpx::validate(uint32_t address, size_t size, uint32_t text_end,
                         size_t pack_count) const {
  if (!size || size > 0x100000000ull - address)
    return false;
  const uint64_t end = uint64_t(address) + size;
  // Mirror mem.h: codecaves, RPX text below data imports and loaded MEM2 data.
  // Never accept the 4 GB reservation itself as proof of writability.
  if (!((address >= mem::kCodeCave && end <= mem::kText) ||
        (address >= 0x02000000 && end <= 0x0df00000) ||
        (address >= 0x10000000 && end <= 0x50000000)))
    return false;
  if (address >= mem::kText) {
    for (const auto &range : writable)
      if (address >= range.begin && end <= range.end)
        return true;
    return false;
  }
  const uint64_t cave = mem::kCodeCave;
  const uint64_t limit =
      cave +
      std::min(pack_count, size_t(mem::kCodeCaveSize / 0x100000)) * 0x100000ull;
  if (!text_end || address < cave || end > limit)
    return false;
  // A codecave must not alias any original section, writable or otherwise.
  for (const auto &range : allocated)
    if (address < range.end && range.begin < end)
      return false;
  return true;
}

bool load_rpx(const BuildPlan &plan, const std::string &path,
              const std::string &cache_directory,
              bool (*load)(const std::string &), LoadedRpx &snapshot,
              std::string &error) {
  snapshot = {};
  error.clear();
  if (plan.packs.empty() && plan.initial_memory.empty())
    return load(path);
  std::vector<uint8_t> raw;
  bool read_ok = false;
  if (FILE *input = host_file::open_path(path.c_str(), "rb")) {  // may be on a USB drive (UWP)
    char buffer[1 << 16];
    for (size_t n; (n = std::fread(buffer, 1, sizeof buffer, input)) > 0;)
      raw.insert(raw.end(), buffer, buffer + n);
    read_ok = !std::ferror(input);
    std::fclose(input);
  }
  if (read_ok && !raw.empty()) {
    static std::atomic<unsigned> serial{0};
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temporary = cache_directory + "/.packs-rpx-" +
                           std::to_string(stamp) + "-" +
                           std::to_string(serial++);
    if (FILE *file = std::fopen(temporary.c_str(), "wbx")) {
      const bool written =
          std::fwrite(raw.data(), 1, raw.size(), file) == raw.size();
      const bool closed = std::fclose(file) == 0;
      if (written && closed) {
        snapshot.hash = sha256(raw.data(), raw.size());
        sections(raw, snapshot);
        const bool result = load(temporary);
        std::remove(temporary.c_str());
        return result;
      }
      std::remove(temporary.c_str());
    }
  }
  error = "Graphic packs disabled: cannot snapshot loaded RPX";
  return load(path);
}
} // namespace packs
