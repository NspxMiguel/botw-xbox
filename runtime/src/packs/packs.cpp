#include "packs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

#if __has_include("packs_generated.inc")
#include "packs_generated.inc"
#else
namespace packs {
const BuildPlan &compiled_plan() {
  static const BuildPlan plan;
  return plan;
}
} // namespace packs
#endif

namespace packs {
namespace {
bool includes(const Field &field, double value) {
  return std::find(field.values.begin(), field.values.end(), value) !=
         field.values.end();
}
} // namespace

std::string sha256(const uint8_t *bytes, size_t size) {
  constexpr uint32_t k[] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
      0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  std::array<uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  auto rotate = [](uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
  };
  auto block = [&](const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 |
             uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
      const auto a = w[i - 15], b = w[i - 2];
      w[i] = w[i - 16] + (rotate(a, 7) ^ rotate(a, 18) ^ (a >> 3)) + w[i - 7] +
             (rotate(b, 17) ^ rotate(b, 19) ^ (b >> 10));
    }
    auto v = h;
    for (int i = 0; i < 64; ++i) {
      uint32_t t1 = v[7] +
                    (rotate(v[4], 6) ^ rotate(v[4], 11) ^ rotate(v[4], 25)) +
                    ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[i] + w[i];
      uint32_t t2 = (rotate(v[0], 2) ^ rotate(v[0], 13) ^ rotate(v[0], 22)) +
                    ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
      for (int j = 7; j > 0; --j)
        v[j] = v[j - 1];
      v[4] += t1;
      v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; ++i)
      h[i] += v[i];
  };
  size_t offset = 0;
  for (; size - offset >= 64; offset += 64)
    block(bytes + offset);
  uint8_t tail[128]{};
  if (size != offset)
    std::memcpy(tail, bytes + offset, size - offset);
  tail[size - offset] = 0x80;
  const size_t padded = size - offset < 56 ? 64 : 128;
  uint64_t bits = uint64_t(size) * 8;
  for (int i = 0; i < 8; ++i)
    tail[padded - 1 - i] = uint8_t(bits >> (8 * i));
  block(tail);
  if (padded == 128)
    block(tail + 64);
  std::string result;
  for (auto word : h)
    for (int shift = 28; shift >= 0; shift -= 4)
      result += "0123456789abcdef"[(word >> shift) & 15];
  return result;
}

bool has_timing_pack(const BuildPlan &plan) {
  for (const auto &pack : plan.packs) {
    std::string name = pack.name;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
      return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
    });
    if (name.find("fps++") != std::string::npos)
      return true;
    for (const auto &field : pack.control)
      if (field.name == "vsyncFrequency")
        return true;
  }
  return false;
}

bool validate_renderer_fields(const BuildPlan &plan, bool textures,
                              bool shaders, std::string &error) {
  error.clear();
  for (const auto &pack : plan.packs) {
    auto reject = [&](const std::string &field) {
      error =
          "Graphic pack '" + pack.name + "': unsupported or invalid " + field;
      return false;
    };
    if (!shaders && !pack.shaders.empty())
      return reject("shader replacements on this renderer");
    for (const auto &shader : pack.shaders)
      if (shader.stage != ShaderStage::vertex &&
          shader.stage != ShaderStage::geometry &&
          shader.stage != ShaderStage::pixel)
        return reject("shader stage");
    for (const auto &field : pack.control) {
      if (field.name != "vsyncFrequency" || field.values.size() != 1 ||
          !std::isfinite(field.values[0]) || field.values[0] < 1 ||
          field.values[0] > 1000)
        return reject("control field " + field.name);
    }
    for (const auto &rule : pack.textures) {
      if (!textures)
        return reject("texture rules on this renderer");
      for (const auto &field : rule.fields) {
        const auto &n = field.name;
        const bool dimension = n == "overwriteWidth" ||
                               n == "overwriteHeight" || n == "overwriteDepth";
        const bool filter = n == "width" || n == "height" || n == "depth" ||
                            n == "formats" || n == "formatsExcluded" ||
                            n == "tilemodes" || n == "tilemodesExcluded" ||
                            n == "inMEM1";
        if ((!dimension && !filter) || field.values.empty())
          return reject("texture field " + n);
        if (dimension &&
            (field.values.size() != 1 || field.values[0] < 1 ||
             field.values[0] > (n == "overwriteDepth" ? 2048 : 16384)))
          return reject("texture field " + n);
        for (double value : field.values)
          if (!std::isfinite(value))
            return reject("texture field " + n);
      }
    }
  }
  return true;
}

bool matches(const TextureRule &rule, const TextureDescription &texture) {
  for (const auto &field : rule.fields) {
    const auto &name = field.name;
    if ((name == "width" || name == "height" || name == "depth") &&
        includes(field, -1))
      continue;
    if (name == "width" && !includes(field, texture.width))
      return false;
    if (name == "height" && !includes(field, texture.height))
      return false;
    if (name == "depth" && !includes(field, texture.depth))
      return false;
    if (name == "formats" && !includes(field, texture.format))
      return false;
    if (name == "formatsExcluded" && includes(field, texture.format))
      return false;
    if (name == "tilemodes" && !includes(field, texture.tile_mode))
      return false;
    if (name == "tilemodesExcluded" && includes(field, texture.tile_mode))
      return false;
    if (name == "inMEM1" && !includes(field, texture.in_mem1 ? 1 : 0))
      return false;
  }
  return true;
}

std::vector<Field> texture_overrides(const BuildPlan &plan,
                                     const TextureDescription &texture) {
  std::vector<Field> result;
  for (const auto &pack : plan.packs) {
    for (const auto &rule : pack.textures) {
      if (!matches(rule, texture))
        continue;
      for (const auto &field : rule.fields) {
        if (field.name.compare(0, 9, "overwrite") != 0)
          continue;
        auto found =
            std::find_if(result.begin(), result.end(),
                         [&](const Field &f) { return f.name == field.name; });
        if (found == result.end())
          result.push_back(field);
        else
          *found = field;
      }
    }
  }
  return result;
}

const ShaderReplacement *find_shader(const BuildPlan &plan, uint64_t base_hash,
                                     uint64_t auxiliary_hash,
                                     ShaderStage stage) {
  const ShaderReplacement *result = nullptr;
  for (const auto &pack : plan.packs) {
    for (const auto &shader : pack.shaders) {
      if (shader.base_hash == base_hash &&
          shader.auxiliary_hash == auxiliary_hash && shader.stage == stage)
        result = &shader;
    }
  }
  return result;
}

bool install(const BuildPlan &plan, const std::string &rpx_sha256,
             const GuestMemory &memory, RendererHooks &renderer,
             std::string &error) {
  error.clear();
  if (plan.packs.empty() && plan.initial_memory.empty())
    return true;
  if (plan.rpx_sha256.empty() || plan.rpx_sha256 != rpx_sha256) {
    error = "Graphic packs were compiled for a different RPX; recompile with "
            "this game and pack selection";
    return false;
  }
  if (!plan.initial_memory.empty() &&
      (!memory.validate_range || !memory.write)) {
    error = "Graphic pack guest memory hooks are missing";
    return false;
  }
  for (const auto &patch : plan.initial_memory) {
    if (patch.bytes.empty() ||
        patch.bytes.size() > uint64_t{0x100000000} - patch.address ||
        !memory.validate_range(memory.context, patch.address,
                               patch.bytes.size())) {
      error = "Graphic pack memory range is not mapped";
      return false;
    }
  }
  if (!renderer.prepare(plan, error))
    return false;
  for (const auto &patch : plan.initial_memory)
    memory.write(memory.context, patch.address, patch.bytes.data(),
                 patch.bytes.size());
  renderer.commit(plan);
  return true;
}
} // namespace packs
