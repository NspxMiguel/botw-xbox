#pragma once
#include "../../../../../../src/packs/packs.h"

namespace d3d12_packs
{
// Published once on the host thread, before guest/GPU workers start.
inline const packs::BuildPlan *active_plan = nullptr;
inline bool overrides_textures = false;

// Preserve the base constructor's FSR-scaled axes unless explicitly overridden.
// Color and depth textures matched by the same rule use this identical path.
template <typename OverwriteInfo>
bool apply_texture_overrides(const packs::BuildPlan &plan,
							 const packs::TextureDescription &original,
							 OverwriteInfo &info)
{
	const auto overrides = packs::texture_overrides(plan, original);
	if (overrides.empty())
		return false;
	if (!info.hasResolutionOverwrite)
	{
		info.width = original.width;
		info.height = original.height;
		info.depth = original.depth > 0 ? original.depth : 1;
	}
	for (const auto &field : overrides)
	{
		if (field.name == "overwriteWidth")
			info.width = int(field.values[0]);
		if (field.name == "overwriteHeight")
			info.height = int(field.values[0]);
		if (field.name == "overwriteDepth")
			info.depth = int(field.values[0]);
	}
	info.hasResolutionOverwrite = true;
	return true;
}

class PackHooks final : public packs::RendererHooks
{
  public:
	using ShaderCompiler = bool (*)(const packs::ShaderReplacement &, std::string &);
	explicit PackHooks(ShaderCompiler compiler) : compiler_(compiler)
	{
	}
	bool prepare(const packs::BuildPlan &plan, std::string &error) override
	{
		// Validate every field before invoking even the CPU shader compiler.
		if (!packs::validate_renderer_fields(plan, true, true, error))
			return false;
		for (const auto &pack : plan.packs)
		{
			for (const auto &shader : pack.shaders)
			{
				if (!compiler_)
				{
					error = "Graphic pack shader compiler is unavailable";
					return false;
				}
				if (!compiler_(shader, error))
				{
					error = "Graphic pack '" + pack.name + "' shader: " + error;
					return false;
				}
			}
		}
		return true;
	}
	void commit(const packs::BuildPlan &plan) override
	{
		overrides_textures = false;
		for (const auto &pack : plan.packs)
			overrides_textures = overrides_textures || !pack.textures.empty();
		active_plan = plan.packs.empty() ? nullptr : &plan;
	}

  private:
	ShaderCompiler compiler_;
};
packs::RendererHooks &hooks();
} // namespace d3d12_packs
