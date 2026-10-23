#pragma once
#include "../../../../../../src/packs/packs.h"
#include <cstring>

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
	bool resolution = false;
	for (const auto &field : overrides)
	{
		if (field.name == "overwriteWidth")
		{
			info.width = int(field.values[0]);
			resolution = true;
		}
		if (field.name == "overwriteHeight")
		{
			info.height = int(field.values[0]);
			resolution = true;
		}
		if (field.name == "overwriteDepth")
		{
			info.depth = int(field.values[0]);
			resolution = true;
		}
		if (field.name == "overwriteFormat")
		{
			info.format = int(field.values[0]);
			info.hasFormatOverwrite = true;
		}
		if (field.name == "overwriteLodBias")
		{
			info.lodBias = int(float(field.values[0]) * 64.0f);
			info.hasLodBias = true;
		}
		if (field.name == "overwriteRelativeLodBias")
		{
			info.relativeLodBias = int(float(field.values[0]) * 64.0f);
			info.hasRelativeLodBias = true;
		}
		if (field.name == "overwriteAnisotropy")
		{
			int exponent = 0;
			for (int value = int(field.values[0]); value > 1; value >>= 1)
				++exponent;
			info.anisotropicLevel = exponent;
		}
	}
	info.hasResolutionOverwrite = info.hasResolutionOverwrite || resolution;
	return true;
}

// Uploads still arrive in the original guest layout. Expand RGB10A2 to the
// RGBA16 render target's layout; views keep the guest channel ordering.
inline std::vector<uint16_t> expand_rgb10a2(const void *pixels, size_t count)
{
	std::vector<uint16_t> result(count * 4);
	for (size_t i = 0; i < count; ++i)
	{
		uint32_t pixel;
		std::memcpy(&pixel, static_cast<const uint8_t *>(pixels) + i * 4, 4);
		for (unsigned channel = 0; channel < 3; ++channel)
		{
			const uint32_t value = (pixel >> (channel * 10)) & 1023;
			result[i * 4 + channel] = uint16_t((value * 65535 + 511) / 1023);
		}
		result[i * 4 + 3] = uint16_t((pixel >> 30) * 21845);
	}
	return result;
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
