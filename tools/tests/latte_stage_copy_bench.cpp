#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
namespace Latte
{
enum class E_DIM : uint32_t
{
	DIM_2D
};
} // namespace Latte
#define LATTE_MAX_VERTEX_BUFFERS 16
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DrawPacket.h"
#include <iostream>
using namespace LattePipeline;
__attribute__((noinline)) static void Copy(DrawBlock &out, const DrawBlock &in)
{
	DrawBlockCopy{}(out, in);
}
int main()
{
	SequenceStageLayouts layouts;
	DrawBlock source{}, destination{};
	source.stageCount = 3;
	for (uint32_t stage = 0; stage < 3; ++stage)
	{
		source.stages[stage] = layouts.Get(static_cast<D3D12Const::Stage>(stage), 123 + stage, [&](StageBlock &out) {
			out.stage = static_cast<D3D12Const::Stage>(stage);
			out.textureCount = out.samplerCount = 8;
			for (uint32_t i = 0; i < 8; ++i)
			{
				out.textures[i].view = {i + 1, 1};
				out.textures[i].swizzle = i;
				out.samplers[i].words[12] = i;
			}
		});
	}
	constexpr uint32_t iterations = 1000000;
	const auto start = std::chrono::steady_clock::now();
	for (uint32_t i = 0; i < iterations; ++i)
	{
		source.stages[0].uniforms[1] = {i * 256, 512};
		Copy(destination, source);
		assert(destination.stages[0].uniforms[1].offset == i * 256);
	}
	assert(DrawBlockEqual{}(source, destination));
	std::cout << "Stable stage layout with changing uniform range: " << std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / iterations << " ns/copy\n";
}
