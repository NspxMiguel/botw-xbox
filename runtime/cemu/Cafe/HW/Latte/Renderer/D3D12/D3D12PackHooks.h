#pragma once
#include "../../../../../../src/packs/packs.h"

namespace d3d12_packs
{
// Published once on the host thread, before guest/GPU workers start.
inline const packs::BuildPlan *active_plan = nullptr;
inline bool overrides_textures = false;

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
