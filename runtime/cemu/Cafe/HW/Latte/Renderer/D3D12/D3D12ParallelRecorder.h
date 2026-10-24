#pragma once

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DrawPacket.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteDeferredIndex.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DescriptorHeaps.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12StageTable.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteChunkWorkers.h"

// Only the coordinator builds these records. No mutable emulator objects cross
// into a worker except the pinned, locked native SRV cache. Resource definitions
// are frozen until all jobs join; native objects live through the common fence.
class LatteTextureViewD3D12;

struct D3D12ResolvedDraw
{
	LattePipeline::CapturedDraw source;
	ID3D12PipelineState *pipeline = nullptr;
	D3D_PRIMITIVE_TOPOLOGY topology{};
	bool blendConstants = false, blendAlphaOnly = false, stencil = false;
	std::array<D3D12_VERTEX_BUFFER_VIEW, LATTE_MAX_VERTEX_BUFFERS> vertices{};
	D3D12_INDEX_BUFFER_VIEW indices{};
	std::shared_ptr<LattePipeline::DeferredIndex> deferredIndices;
	std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> targets{};
	uint32 targetCount = 0;
	D3D12_CPU_DESCRIPTOR_HANDLE depth{};
	std::array<D3D12StageTableDesc, 3> stages{};
	struct TextureSource
	{
		LatteTextureViewD3D12 *view = nullptr;
		uint32 swizzle = 0;
	};
	std::array<std::array<TextureSource, D3D12Const::kMaxSRVsPerStage>, 3> textures{};
	std::vector<D3D12_RESOURCE_BARRIER> barriers;

	D3D12ResolvedDraw(const LattePipeline::DrawPacket &packet, const LattePipeline::DrawBlock &block, const LattePipeline::Arena &arena)
		: source(packet, block, arena)
	{
	}
};

class D3D12ParallelRecorder
{
  public:
	D3D12ParallelRecorder(ID3D12Device *device, ID3D12Fence *fence, ID3D12RootSignature *root,
						  D3D12_CPU_DESCRIPTOR_HANDLE nullCBV, D3D12_CPU_DESCRIPTOR_HANDLE nullSRV,
						  D3D12_CPU_DESCRIPTOR_HANDLE nullUAV, D3D12_GPU_VIRTUAL_ADDRESS zero);
	~D3D12ParallelRecorder();
	LattePipeline::ChunkTiming Record(std::span<const D3D12ResolvedDraw> draws, uint64 fence, unsigned requested, bool profile);
	void AppendLists(std::vector<ID3D12CommandList *> &lists);
	void Submitted(uint64 fence);

  private:
	struct Worker;
	std::array<std::unique_ptr<Worker>, 4> m_workers;
	// Declared last so threads are joined before native worker storage is freed.
	LattePipeline::ChunkWorkers m_pool;
	unsigned m_count = 0;
};
