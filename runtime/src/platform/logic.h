#pragma once
#include "platform.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace platform {
// Frequency conversion without non-MSVC 128-bit integers. The result must fit
// uint64_t.
inline uint64_t scale_ticks(uint64_t ticks, uint64_t frequency, uint32_t rate) {
  if (!frequency)
    return 0;
  // Clocks used here have frequency <= 1 GHz, so remainder * rate fits
  // uint64_t.
  return ticks / frequency * rate + ticks % frequency * rate / frequency;
}
// Windows AV access kinds: 0 = read, 1 = write, 8 = execute. A zero page means
// the fault is not demand-committable (including the guest null guard).
inline uintptr_t guest_commit_page(uintptr_t base, uintptr_t address,
                                   uintptr_t access) {
  if (!base || address < base || address - base < kNullGuard ||
      address - base >= kGuestSize || access > 1)
    return 0;
  return address & ~uintptr_t(0xFFFF);
}
struct Roots {
  std::string game;
  std::string user;
};
// The UWP host passes ApplicationData::Current().LocalFolder().Path() and
// optionally an authorized external folder. Call before guest startup; never
// use the process cwd.
inline Roots local_roots(std::string local_state,
                         std::string external_game = {}) {
  return {external_game.empty() ? local_state + "/game"
                                : std::move(external_game),
          std::move(local_state)};
}
inline std::string guest_path(std::string path, std::string_view cwd,
                              const Roots &roots) {
  if (path.empty() || path.front() != '/')
    path = std::string(cwd) + "/" + path;
  // Normalize before selecting a mount. Backslashes/colons cannot become host
  // paths.
  std::vector<std::string> parts;
  size_t pos = 0;
  while (pos < path.size()) {
    size_t end = path.find('/', pos);
    if (end == std::string::npos)
      end = path.size();
    std::string part = path.substr(pos, end - pos);
    if (part.find_first_of("\\:") != std::string::npos ||
        part.find('\0') != std::string::npos)
      return {};
    if (part == "..") {
      if (parts.size() <= 2)
        return {}; // cannot leave a guest volume
      parts.pop_back();
    } else if (!part.empty() && part != ".")
      parts.push_back(std::move(part));
    pos = end + 1;
  }
  if (parts.size() < 2 || parts[0] != "vol")
    return {};
  const auto &volume = parts[1];
  const std::string *root = nullptr;
  if (volume == "save")
    root = &roots.user;
  else if (volume == "content" || volume == "code" || volume == "meta" ||
           volume == "aoc")
    root = &roots.game;
  if (!root || root->empty())
    return {};
  std::string result = *root;
  for (size_t i = 1; i < parts.size(); ++i)
    result += "/" + parts[i];
  return result;
}
} // namespace platform
