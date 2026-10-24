// SPDX-License-Identifier: MIT
#pragma once
#include "platform.h"
#include <algorithm>
#include <cstdint>

namespace platform {
// UWP's process limit covers CPU, GPU and driver allocations together,
// regardless of physical RAM. The live MemoryManager limit can be lower
// (notably App mode).
constexpr uint64_t kUwpGameMemoryLimit = 5ull << 30;
constexpr uint64_t kUwpStartupReserve = 256ull << 20;
constexpr uint64_t kGuestLowCommitEnd = 0x50000000;
constexpr uint64_t kGuestHighCommitStart = 0xA0000000;
constexpr uint64_t guest_initial_commit_bytes(bool lazy_commit) {
  return lazy_commit ? kGuestLowCommitEnd - kNullGuard + kGuestSize -
                           kGuestHighCommitStart
                     : kGuestSize - kNullGuard;
}
constexpr uint64_t uwp_memory_limit(uint64_t reported_limit) {
  return std::min(reported_limit, kUwpGameMemoryLimit);
}
// A startup admission check, not a bound on future renderer/driver allocations
// or later guest commits. Subtraction avoids overflow for bad/large readings.
constexpr bool uwp_memory_admits_guest(uint64_t reported_limit, uint64_t usage,
                                       bool lazy_commit) {
  const auto limit = uwp_memory_limit(reported_limit);
  const auto guest = guest_initial_commit_bytes(lazy_commit);
  return usage <= limit && guest <= limit - usage &&
         kUwpStartupReserve <= limit - usage - guest;
}
} // namespace platform
