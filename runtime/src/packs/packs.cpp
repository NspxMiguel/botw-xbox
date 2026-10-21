#include "sha256.h"
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
  return sha256_bytes(bytes, size);
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
  // Merge first so a later identity can cancel an earlier size override.
  result.erase(std::remove_if(result.begin(), result.end(),
                              [&](const Field &field) {
                                if (field.values.size() != 1)
                                  return false;
                                const double value =
                                    std::trunc(field.values[0]);
                                return (field.name == "overwriteWidth" &&
                                        value == texture.width) ||
                                       (field.name == "overwriteHeight" &&
                                        value == texture.height) ||
                                       (field.name == "overwriteDepth" &&
                                        value == std::max(1u, texture.depth));
                              }),
               result.end());
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
