#include "packs.h"

#include <algorithm>
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
