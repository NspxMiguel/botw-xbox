// Synthetic runtime contract tests; no graphics API or game assets required.
#include "packs.h"

#include <cassert>
#include <cstring>
#include <string>
#include <vector>

struct Renderer final : packs::RendererHooks {
  bool accept = true;
  int prepares = 0, commits = 0;
  bool prepare(const packs::BuildPlan &, std::string &error) override {
    ++prepares;
    if (!accept)
      error = "Unsupported test capability";
    return accept;
  }
  void commit(const packs::BuildPlan &) override { ++commits; }
};
struct Memory {
  uint8_t bytes[32]{};
  int writes = 0;
  static bool validate(void *, uint32_t address, size_t size) {
    return address >= 0x1000 && uint64_t{address} + size <= 0x1020;
  }
  static void write(void *context, uint32_t address, const uint8_t *bytes,
                    size_t size) {
    auto &self = *static_cast<Memory *>(context);
    ++self.writes;
    std::memcpy(self.bytes + address - 0x1000, bytes, size);
  }
};
int main() {
  packs::BuildPlan plan;
  plan.rpx_sha256 = "synthetic";
  packs::Pack pack;
  pack.name = "Synthetic resolution";
  pack.textures.push_back({{{"width", {1280}},
                            {"formats", {26, 27}},
                            {"formatsExcluded", {27}},
                            {"tilemodes", {4}},
                            {"inMEM1", {0}},
                            {"overwriteWidth", {1920}},
                            {"overwriteHeight", {1080}}}});
  pack.textures.push_back({{{"width", {1280}}, {"overwriteWidth", {2560}}}});
  pack.shaders.push_back({1, 2, packs::ShaderStage::pixel, "synthetic shader"});
  plan.packs.push_back(pack);
  packs::TextureDescription texture{1280, 720, 1, 26, 4, false};
  assert(packs::matches(pack.textures[0], texture));
  texture.format = 27;
  assert(!packs::matches(pack.textures[0], texture));
  texture.format = 26;
  texture.in_mem1 = true;
  assert(!packs::matches(pack.textures[0], texture));
  texture.in_mem1 = false;
  auto fields = packs::texture_overrides(plan, texture);
  assert(fields.size() == 2 && fields[0].values[0] == 2560 &&
         fields[1].values[0] == 1080);
  assert(packs::find_shader(plan, 1, 2, packs::ShaderStage::pixel));
  assert(!packs::find_shader(plan, 1, 3, packs::ShaderStage::pixel));
  assert(!packs::find_shader(plan, 1, 2, packs::ShaderStage::vertex));
  Memory memory;
  Renderer renderer;
  packs::GuestMemory hooks{&memory, Memory::validate, Memory::write};
  std::string error;
  plan.initial_memory.push_back({0x1000, {0x60, 0, 0, 0}});
  assert(!packs::install(plan, "wrong hash", hooks, renderer, error));
  assert(memory.writes == 0 && renderer.prepares == 0);
  plan.initial_memory.push_back({0x2000, {1}});
  assert(!packs::install(plan, "synthetic", hooks, renderer, error));
  assert(memory.writes == 0 && renderer.prepares == 0);
  plan.initial_memory.pop_back();
  renderer.accept = false;
  assert(!packs::install(plan, "synthetic", hooks, renderer, error));
  assert(memory.writes == 0 && renderer.commits == 0);
  renderer.accept = true;
  assert(packs::install(plan, "synthetic", hooks, renderer, error));
  assert(error.empty() && memory.writes == 1 && memory.bytes[0] == 0x60 &&
         renderer.commits == 1);
}
