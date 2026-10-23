#include "../../../cemu/Cafe/HW/Latte/Renderer/D3D12/D3D12PackHooks.h"
#include "../../latte/pack_hooks_metal.h"
#include "../loaded_rpx.h"
#include "../packs.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>

namespace {
unsigned compilations = 0;
bool fake_compiler(const packs::ShaderReplacement &, std::string &) {
  ++compilations;
  return true;
}
struct Memory {
  uint8_t bytes[8]{};
  unsigned writes = 0;
  static bool validate(void *, uint32_t address, size_t size) {
    return address >= 0x1000 && size <= 0x1008ull - address && address < 0x1008;
  }
  static void write(void *context, uint32_t address, const uint8_t *bytes,
                    size_t size) {
    auto &self = *static_cast<Memory *>(context);
    ++self.writes;
    std::memcpy(self.bytes + (address - 0x1000), bytes, size);
  }
};
void hash(const std::string &input, const std::string &expected) {
  assert(packs::sha256(reinterpret_cast<const uint8_t *>(input.data()),
                       input.size()) == expected);
}
std::vector<uint8_t> expected_rpx;
std::string original_path, snapshot_path;
bool load_snapshot(const std::string &path) {
  snapshot_path = path;
  assert(path != original_path);
  // Changing the source cannot change the snapshot given to the real loader.
  std::ofstream(original_path, std::ios::binary) << "changed source";
  std::ifstream file(path, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
  assert(bytes == expected_rpx);
  return true;
}
bool load_original(const std::string &path) {
  assert(path == original_path);
  return true;
}
void be32(size_t offset, uint32_t value) {
  for (int i = 0; i < 4; ++i)
    expected_rpx[offset + i] = uint8_t(value >> (24 - i * 8));
}
void texture_scaling() {
  struct Dimensions {
    int width, height, depth;
    bool hasResolutionOverwrite;
    int format = 0, lodBias = 0, relativeLodBias = 0, anisotropicLevel = -1;
    bool hasFormatOverwrite = false, hasLodBias = false, hasRelativeLodBias = false;
  };
  packs::BuildPlan plan;
  plan.packs.push_back({"synthetic graphics", {}, {}, {}});
  auto &rules = plan.packs[0].textures;
  const packs::TextureDescription target{1280, 720, 1, 1, 0, false};
  const packs::TextureDescription shadow{1024, 1024, 1, 1, 0, false};
  // Native preset expressions truncate to identity, including pack rounding.
  rules = {{{{"width", {1280}},
             {"height", {720}},
             {"overwriteWidth", {1280.49}},
             {"overwriteHeight", {720}},
             {"overwriteDepth", {1}}}}};
  assert(packs::texture_overrides(plan, target).empty());
  Dimensions scaled{1920, 1080, 1, true}; // Built-in scale 1.5.
  assert(!d3d12_packs::apply_texture_overrides(plan, target, scaled));
  assert(scaled.width == 1920 && scaled.height == 1080);
  rules.push_back({{{"width", {1024}},
                    {"height", {1024}},
                    {"overwriteWidth", {3072}},
                    {"overwriteHeight", {3072}}}});
  Dimensions shadow_size{1024, 1024, 1, false};
  assert(d3d12_packs::apply_texture_overrides(plan, shadow, shadow_size));
  assert(shadow_size.width == 3072 && shadow_size.height == 3072 &&
         shadow_size.hasResolutionOverwrite);
  assert(!d3d12_packs::apply_texture_overrides(plan, target, scaled));
  assert(scaled.width == 1920 && scaled.height == 1080);
  // A partial explicit override preserves scaling on the other axes.
  rules.push_back(
      {{{"width", {1280}}, {"height", {720}}, {"overwriteWidth", {2560}}}});
  assert(d3d12_packs::apply_texture_overrides(plan, target, scaled));
  assert(scaled.width == 2560 && scaled.height == 1080 && scaled.depth == 1);
  const packs::TextureDescription other{640, 360, 1, 1, 0, false};
  Dimensions other_scaled{960, 540, 1, true};
  assert(!d3d12_packs::apply_texture_overrides(plan, other, other_scaled));
  assert(other_scaled.width == 960 && other_scaled.height == 540);
  // A later identity cancels an earlier override before identity removal.
  rules.push_back({{{"width", {1280}}, {"overwriteWidth", {1280}}}});
  assert(packs::texture_overrides(plan, target).empty());
  // Identity filtering also works for wildcard matches and normalized depth.
  rules = {{{{"width", {-1}}, {"overwriteDepth", {1}}}}};
  auto zero_depth = target;
  zero_depth.depth = 0;
  assert(packs::texture_overrides(plan, zero_depth).empty());
  // Every partial rule applies identically to matched color and depth buffers.
  // The constructor calls this helper for both, using their original formats.
  const packs::TextureDescription depth_target{1280, 720, 1, 0x811, 0, false};
  const packs::TextureDescription color_target{1280, 720, 1, 0x1a, 0, false};
  const char *axes[] = {"overwriteWidth", "overwriteHeight", "overwriteDepth"};
  const int sizes[] = {2560, 1440, 4};
  for (int axis = 0; axis < 3; ++axis) {
    rules = {{{{"width", {1280}},
               {"height", {720}},
               {axes[axis], {double(sizes[axis])}}}}};
    for (const auto &texture : {color_target, depth_target}) {
      Dimensions partial{1920, 1080, 1, true};
      assert(d3d12_packs::apply_texture_overrides(plan, texture, partial));
      assert(partial.width == (axis == 0 ? 2560 : 1920));
      assert(partial.height == (axis == 1 ? 1440 : 1080));
      assert(partial.depth == (axis == 2 ? 4 : 1));
      assert(partial.hasResolutionOverwrite);
      // Without built-in scaling, overwriteInfo's dimensions start at zero.
      Dimensions unscaled{0, 0, 0, false};
      assert(d3d12_packs::apply_texture_overrides(plan, texture, unscaled));
      assert(unscaled.width == (axis == 0 ? 2560 : 1280));
      assert(unscaled.height == (axis == 1 ? 1440 : 720));
      assert(unscaled.depth == (axis == 2 ? 4 : 1));
      assert(unscaled.hasResolutionOverwrite);
    }
  }
  // An identity on one axis also preserves FSR when another axis changes.
  rules = {{{{"width", {1280}},
             {"overwriteWidth", {2560}},
             {"overwriteHeight", {720}}}}};
  Dimensions partial{1920, 1080, 3, true};
  assert(d3d12_packs::apply_texture_overrides(plan, target, partial));
  assert(partial.width == 2560 && partial.height == 1080 && partial.depth == 3);
  rules = {{{{"width", {-1}}, {"overwriteWidth", {2560}}}}};
  Dimensions normalized{0, 0, 0, false};
  assert(d3d12_packs::apply_texture_overrides(plan, zero_depth, normalized));
  assert(normalized.width == 2560 && normalized.height == 720 &&
         normalized.depth == 1);
  // Non-size overrides must never turn off FSR or create a size override.
  const packs::TextureDescription gui{1280, 720, 1, 0x19, 0, false};
  rules = {{{{"formats", {0x19}}, {"overwriteFormat", {0x1f}},
             {"overwriteAnisotropy", {8}}, {"overwriteLodBias", {-0.5}},
             {"overwriteRelativeLodBias", {0.25}}}}};
  std::string error;
  assert(packs::validate_renderer_fields(plan, true, true, error));
  Dimensions gui_scaled{1920, 1080, 1, true};
  assert(d3d12_packs::apply_texture_overrides(plan, gui, gui_scaled));
  assert(gui_scaled.width == 1920 && gui_scaled.height == 1080);
  assert(gui_scaled.format == 0x1f && gui_scaled.hasFormatOverwrite);
  assert(gui_scaled.anisotropicLevel == 3);
  assert(gui_scaled.hasLodBias && gui_scaled.lodBias == -32);
  assert(gui_scaled.hasRelativeLodBias && gui_scaled.relativeLodBias == 16);
  Dimensions gui_native{0, 0, 0, false};
  assert(d3d12_packs::apply_texture_overrides(plan, gui, gui_native));
  assert(!gui_native.hasResolutionOverwrite);
  for (int level : {1, 2, 4, 8, 16}) {
    rules = {{{{"overwriteAnisotropy", {double(level)}}}}};
    assert(packs::validate_renderer_fields(plan, true, true, error));
    Dimensions info{0, 0, 0, false};
    d3d12_packs::apply_texture_overrides(plan, gui, info);
    assert((1 << info.anisotropicLevel) == level);
  }
  for (const auto &field : std::vector<packs::Field>{
           {"overwriteAnisotropy", {3}}, {"overwriteLodBias", {1000}},
           {"overwriteFormat", {0x41a}}}) {
    rules = {{{field}}};
    assert(!packs::validate_renderer_fields(plan, true, true, error));
  }
  const uint32_t pixels[] = {0, 0xffffffff, 1023, 1023u << 10,
                              1023u << 20, 1u << 30, 512};
  const auto rgba = d3d12_packs::expand_rgb10a2(pixels, 7);
  assert(rgba.size() == 28 && rgba[0] == 0 && rgba[3] == 0);
  for (int channel = 0; channel < 4; ++channel) assert(rgba[4 + channel] == 65535);
  assert(rgba[8] == 65535 && rgba[9] == 0 && rgba[10] == 0);
  assert(rgba[12] == 0 && rgba[13] == 65535 && rgba[14] == 0);
  assert(rgba[16] == 0 && rgba[17] == 0 && rgba[18] == 65535);
  assert(rgba[23] == 21845 && rgba[24] == 32800);
  std::cout << "texture overrides: identity removal and per-texture scale "
               "precedence passed\n";
}
} // namespace
int main(int argc, char **argv) {
  assert(argc == 2);
  texture_scaling();
  hash("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  hash("abc",
       "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  hash(std::string(1000000, 'a'),
       "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  std::cout << "SHA-256: 4 vectors passed\n";

  packs::BuildPlan plan;
  plan.external_timing = true;
  assert(packs::has_timing_pack(plan));
  plan.external_timing = false;
  assert(!packs::has_timing_pack(plan));
  plan.rpx_sha256 = "test hash";
  plan.packs.push_back({"memory only", {}, {}, {}});
  plan.initial_memory.push_back({0x1000, {1, 2, 3, 4}});
  plan.initial_memory.push_back({0x1007, {5, 6}});
  Memory memory;
  packs::GuestMemory guest{&memory, Memory::validate, Memory::write};
  auto &hooks = latte::metal_pack_hooks();
  std::string error;
  assert(!packs::install(plan, plan.rpx_sha256, guest, hooks, error));
  assert(memory.writes == 0);
  plan.initial_memory.pop_back();
  assert(packs::install(plan, plan.rpx_sha256, guest, hooks, error));
  assert(memory.writes == 1 && memory.bytes[0] == 1 && memory.bytes[3] == 4);
  std::cout
      << "install: out-of-range rejected transactionally; in-range applied\n";

  plan.packs[0].control.push_back({"unknownControl", {60}});
  assert(!hooks.prepare(plan, error) &&
         error.find("unknownControl") != std::string::npos);
  assert(!packs::validate_renderer_fields(plan, true, true, error));
  plan.packs[0].control = {{"vsyncFrequency", {120}}};
  assert(hooks.prepare(plan, error) && packs::has_timing_pack(plan));
  plan.packs[0].textures = {{{{"width", {1280}}, {"overwriteWidth", {3840}}}}};
  assert(packs::validate_renderer_fields(plan, true, true, error));
  assert(!hooks.prepare(plan, error));
  plan.packs[0].textures[0].fields.push_back({"unknownTexture", {1}});
  assert(!packs::validate_renderer_fields(plan, true, true, error));
  assert(error.find("unknownTexture") != std::string::npos);
  d3d12_packs::PackHooks d3d12_hooks{fake_compiler};
  plan.packs[0].shaders.push_back(
      {1, 2, packs::ShaderStage::pixel, "test shader"});
  assert(!d3d12_hooks.prepare(plan, error));
  assert(compilations == 0 && !d3d12_packs::active_plan);
  plan.packs[0].textures[0].fields.pop_back();
  assert(d3d12_hooks.prepare(plan, error));
  assert(compilations == 1 && !d3d12_packs::active_plan);
  d3d12_hooks.commit(plan);
  assert(d3d12_packs::active_plan == &plan && d3d12_packs::overrides_textures);
  d3d12_hooks.commit({});
  assert(!d3d12_packs::active_plan && !d3d12_packs::overrides_textures);
  std::cout << "D3D12/Metal hooks prepare: unknown fields rejected before "
               "compilation; commit publishes plan\n";

  original_path = std::string(argv[1]) + "/synthetic.rpx";
  expected_rpx.resize(52 + 3 * 40);
  be32(0, 0x7f454c46);
  be32(32, 52);
  expected_rpx[47] = 40;
  expected_rpx[49] = 3;
  // text, writable data, read-only section immediately beyond the text.
  be32(52 + 4, 1);
  be32(52 + 8, 6);
  be32(52 + 12, 0x02000000);
  be32(52 + 20, 16);
  be32(92 + 4, 8);
  be32(92 + 8, 3);
  be32(92 + 12, 0x10000000);
  be32(92 + 20, 16);
  be32(132 + 4, 1);
  be32(132 + 8, 2);
  be32(132 + 12, 0x02000010);
  be32(132 + 20, 16);
  {
    std::ofstream file(original_path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(expected_rpx.data()),
               expected_rpx.size());
  }
  packs::LoadedRpx loaded;
  assert(packs::load_rpx(plan, original_path, argv[1], load_snapshot, loaded,
                         error));
  assert(error.empty() && loaded.hash == packs::sha256(expected_rpx.data(),
                                                       expected_rpx.size()));
  assert(!std::ifstream(snapshot_path));
  assert(loaded.validate(0x02000000, 16, 0x02000010, 1));
  assert(loaded.validate(0x10000000, 16, 0x02000010, 1));
  assert(!loaded.validate(0x1000000f, 2, 0x02000010, 1));
  assert(!loaded.validate(0x10001000, 4, 0x02000010, 1));
  assert(!loaded.validate(0x02000010, 4, 0x02000010, 1));
  assert(!loaded.validate(0x02000020, 16, 0x02000010, 1));
  assert(loaded.validate(0x01800000, 16, 0x02000010, 1));
  assert(loaded.validate(0x018ffffc, 4, 0x02000010, 1));
  assert(!loaded.validate(0x018ffffc, 8, 0x02000010, 1));
  assert(!loaded.validate(0x01800000, 4, 0x02000010, 0));
  assert(!loaded.validate(0x017ffffc, 8, 0x02000010, 1));
  assert(loaded.validate(0x01fffffc, 4, 0x02000010, 100));
  assert(!loaded.validate(0x01fffffc, 8, 0x02000010, 100));
  loaded.allocated.push_back({0x01800000, 0x01800010});
  assert(!loaded.validate(0x01800000, 4, 0x02000010, 1));
  assert(!loaded.validate(0x0180000c, 8, 0x02000010, 1));
  assert(!loaded.validate(0x0df00000, 4, 0x02000010, 1));
  assert(!loaded.validate(0x0e100000, 4, 0x02000010, 1));
  assert(!loaded.validate(0xfffffff0, 32, 0x02000010, 1));
  assert(!loaded.validate(0, 0, 0x02000010, 1));
  assert(packs::load_rpx({}, original_path, argv[1], load_original, loaded,
                         error));
  assert(loaded.hash.empty() && error.empty());
  assert(packs::install({}, "", {}, hooks, error));
  assert(packs::load_rpx(plan, original_path, "/nonexistent/cache",
                         load_original, loaded, error));
  assert(loaded.hash.empty() && !error.empty());
  std::remove(original_path.c_str());
  std::cout << "loaded RPX: snapshot/hash identity, writable bounds, codecave, "
               "empty plan and fallback passed\n";
}
