#pragma once
#include "../loader.h"
#include "../log.h"
#include "../mem.h"
#include "../mods/catalog.h"
#include "../os.h"
#include <cstdlib>
#include "loaded_rpx.h"

namespace packs {
// The compiled plan minus the packs switched off in the launcher's Mods screen (mods/enabled.txt).
inline const BuildPlan &selected_plan() {
  static const BuildPlan plan = [] {
    if (compiled_plan().packs.empty())
      return compiled_plan();
    try {
      const char *root = std::getenv("BOTW_MODS_ROOT");
      mods::ModCatalog catalog;
      catalog.load(root ? root : g_user_root + "/mods", compiled_plan());
      return mods::filtered_plan(compiled_plan(), catalog);
    } catch (const std::exception &e) {
      LOG("Mods selection unreadable, using every pack: %s", e.what());
      return compiled_plan();
    }
  }();
  return plan;
}
inline void install_loaded(LoadedRpx &snapshot, RendererHooks &renderer) {
  const GuestMemory memory{
      &snapshot,
      [](void *context, uint32_t address, size_t size) {
        const auto &module = loader::main_module();
        if (size > uint64_t{0x100000000} - address)
          return false;
        const uint64_t end = uint64_t(address) + size;
        const bool text = address >= mem::kText && end <= mem::kDataImports;
        const bool data = address >= mem::kMem2 && end <= module.data_end &&
                          end <= uint64_t(mem::kMem2) + mem::kMem2Size;
        return g_mem && (text || data) &&
               static_cast<LoadedRpx *>(context)->validate(
                   address, size, loader::main_module().text_end,
                   compiled_plan().packs.size());
      },
      [](void *, uint32_t address, const uint8_t *bytes, size_t size) {
        std::memcpy(mem::host(address), bytes, size);
      }};
  std::string error;
  if (!install(selected_plan(), snapshot.hash, memory, renderer, error)) {
    LOG("Graphic packs disabled: %s", error.c_str());
    return;
  }
  static bool timing_logged = false;
  if (has_timing_pack(selected_plan()) && !timing_logged) {
    timing_logged = true;
    LOG("Graphic pack FPS++/vsyncFrequency detected; game/fps.cpp g_target_fps "
        "remains authoritative (no additional pack pacing)");
  }
}
} // namespace packs
