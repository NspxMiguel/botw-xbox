#pragma once
#include "packs.h"

namespace packs {
// Owned source snapshot and writable section bounds, captured before
// relocation.
struct LoadedRpx {
  struct Range {
    uint32_t begin;
    uint64_t end;
  };
  std::string hash;
  std::vector<Range> writable;
  std::vector<Range> allocated;
  bool validate(uint32_t address, size_t size, uint32_t text_end,
                size_t pack_count) const;
};
// The loader only accepts paths. Load a private, exclusive snapshot of the
// bytes we hashed, then remove it. On snapshot failure load the original
// without packs.
bool load_rpx(const BuildPlan &plan, const std::string &path,
              const std::string &cache_directory,
              bool (*load)(const std::string &), LoadedRpx &snapshot,
              std::string &error);
} // namespace packs
