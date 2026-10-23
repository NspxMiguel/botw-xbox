#pragma once

#include "Cafe/HW/Latte/Renderer/D3D12/LattePipeline.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12BindingModel.h"

namespace LattePipeline
{
struct UniformRange
{
	uint32_t offset = 0, size = 0;
};
struct TextureBinding
{
	ResourceId view;
	uint32_t swizzle = 0;
	int32_t reg = -1;
	Latte::E_DIM nullDimension{};
};
struct StageBlock
{
	D3D12Const::Stage stage{};
	UniformRange uniforms[D3D12Const::kMaxCBVsPerStage]{};
	TextureBinding textures[D3D12Const::kMaxSRVsPerStage]{};
	D3D12_SAMPLER_DESC samplers[D3D12Const::kMaxSamplersPerStage]{};
	Span uniformBytes;
	int32_t uniformReg = -1;
	uint32_t samplerCount = 1, textureCount = 0;
	bool streamout = false;
};
struct VertexRange
{
	uint32_t offset = 0, size = 0, stride = 0;
	bool bound = false;
};
struct DrawBlock
{
	StageBlock stages[3];
	VertexRange vertices[LATTE_MAX_VERTEX_BUFFERS]{};
	uint32_t stageCount = 0;
};
struct DrawBlockEqual
{
	bool operator()(const DrawBlock &a, const DrawBlock &b) const
	{
		if (a.stageCount != b.stageCount || std::memcmp(a.vertices, b.vertices, sizeof(a.vertices)) != 0)
			return false;
		for (uint32_t i = 0; i < a.stageCount; ++i)
		{
			const auto &x = a.stages[i], &y = b.stages[i];
			if (x.stage != y.stage || x.uniformReg != y.uniformReg || x.samplerCount != y.samplerCount || x.textureCount != y.textureCount || x.streamout != y.streamout)
				return false;
			if (std::memcmp(x.uniforms, y.uniforms, sizeof(x.uniforms)) != 0 ||
				std::memcmp(x.textures, y.textures, x.textureCount * sizeof(*x.textures)) != 0 ||
				std::memcmp(x.samplers, y.samplers, x.samplerCount * sizeof(*x.samplers)) != 0)
				return false;
		}
		return true;
	}
};
struct DrawPacket
{
	ResourceId pipeline, framebuffer, indices;
	ResourceId state;
	Span uniformBytes[3];
	uint32_t baseVertex = 0, baseInstance = 0, instanceCount = 0, count = 0, indexCount = 0;
	Span runtime;
	D3D12_VIEWPORT viewport{};
	D3D12_RECT scissor{};
	float blendFactor[4]{};
	uint32_t stencilRef = 0;

	uint32_t indexType = 0;
	bool streamout = false;
};
static_assert(sizeof(DrawPacket) <= 208);
} // namespace LattePipeline
