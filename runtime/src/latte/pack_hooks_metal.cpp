#include "pack_hooks_metal.h"
namespace latte {
namespace {
// Metal has no replacement/texture publication API outside its owned subtree.
// Memory-only and advisory timing plans are safe; reject other capabilities
// transactionally rather than silently accepting a partially applied pack.
class MetalPackHooks final : public packs::RendererHooks {
public:
  bool prepare(const packs::BuildPlan &plan, std::string &error) override {
    return packs::validate_renderer_fields(plan, false, false, error);
  }
  void commit(const packs::BuildPlan &plan) override { plan_ = &plan; }

private:
  const packs::BuildPlan *plan_ = nullptr;
};
} // namespace
packs::RendererHooks &metal_pack_hooks() {
  static MetalPackHooks hooks;
  return hooks;
}
} // namespace latte
