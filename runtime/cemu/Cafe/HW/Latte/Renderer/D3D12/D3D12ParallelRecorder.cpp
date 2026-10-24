#ifdef ENABLE_D3D12
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ParallelRecorder.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureViewD3D12.h"

struct D3D12ParallelRecorder::Worker final : D3D12SubmissionTracker
{
	ID3D12Device *device;
	ID3D12Fence *fence;
	ID3D12RootSignature *root;
	D3D12_CPU_DESCRIPTOR_HANDLE nullCBV, nullSRV, nullUAV;
	D3D12_GPU_VIRTUAL_ADDRESS zero;
	HANDLE event = nullptr;
	uint64 recording = 0;
	D3D12UploadRing uploads;
	D3D12GpuViewHeap views;
	D3D12StagingDescriptorHeap stagingViews, stagingSamplers;
	D3D12SamplerCache samplers;
	D3D12StageTableCache<> tables[3];
	std::array<D3D12SamplerTableMemo, 3> samplerMemo;
	struct UniformUpload
	{
		std::vector<std::byte> bytes;
		D3D12_GPU_VIRTUAL_ADDRESS address = 0;
	};
	std::array<UniformUpload, 3> uniforms;
	uint64 uniformBytes = 0;
	D3D12_CPU_DESCRIPTOR_HANDLE cbvs[3][D3D12Const::kMaxCBVsPerStage]{};
	struct Slot
	{
		ComPtr<ID3D12CommandAllocator> allocator;
		ComPtr<ID3D12GraphicsCommandList> list;
		std::vector<std::unique_ptr<D3D12GpuSamplerHeap>> samplers;
		uint64 fence = 0;
	};
	std::deque<std::unique_ptr<Slot>> slots;
	Slot *active = nullptr;
	size_t samplerHeap = 0;

	Worker(ID3D12Device *d, ID3D12Fence *f, ID3D12RootSignature *r, D3D12_CPU_DESCRIPTOR_HANDLE cbv,
		   D3D12_CPU_DESCRIPTOR_HANDLE srv, D3D12_CPU_DESCRIPTOR_HANDLE uav, D3D12_GPU_VIRTUAL_ADDRESS z)
		: device(d), fence(f), root(r), nullCBV(cbv), nullSRV(srv), nullUAV(uav), zero(z),
		  uploads(d, this, 4 * 1024 * 1024, "ChunkUploads"), views(d, this, 65536),
		  stagingViews(d, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 256, "ChunkViews"),
		  stagingSamplers(d, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 256, "ChunkSamplers"), samplers(d, &stagingSamplers)
	{
		event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
		if (!event)
			throw std::runtime_error("Latte chunk fence event creation failed");
	}
	~Worker()
	{
		if (event)
			CloseHandle(event);
	}
	uint64 GetRecordingSubmissionId() const override
	{
		return recording;
	}
	uint64 GetCompletedSubmissionId() override
	{
		const auto completed = fence->GetCompletedValue();
		if (completed == UINT64_MAX)
			throw std::runtime_error("Latte chunk device removed");
		return completed;
	}
	void WaitForSubmission(uint64 value) override
	{
		if (value >= recording)
			throw std::logic_error("Latte chunk cannot wait for its unsubmitted fence");
		if (GetCompletedSubmissionId() >= value)
			return;
		D3D12_ThrowIfFailed(fence->SetEventOnCompletion(value, event), "Chunk SetEventOnCompletion");
		if (WaitForSingleObjectEx(event, INFINITE, FALSE) != WAIT_OBJECT_0)
			throw std::runtime_error("Latte chunk fence wait failed");
	}
	void Begin(uint64 value)
	{
		recording = value;
		samplerMemo = {};
		uniformBytes = 0;
		for (auto &uniform : uniforms)
			uniform.address = 0;
		const auto completed = GetCompletedSubmissionId();
		uploads.Retire(completed);
		views.Retire(completed);
		active = nullptr;
		for (auto &slot : slots)
			if (slot->fence <= completed)
			{
				active = slot.get();
				break;
			}
		if (!active)
		{
			// Bound allocator/sampler memory even when the GPU falls behind.
			if (slots.size() >= 4)
			{
				WaitForSubmission(slots.front()->fence);
				active = slots.front().get();
			}
			else
			{
				auto slot = std::make_unique<Slot>();
				D3D12_ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot->allocator)), "Chunk CreateCommandAllocator");
				D3D12_ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot->allocator.Get(), nullptr, IID_PPV_ARGS(&slot->list)), "Chunk CreateCommandList");
				D3D12_ThrowIfFailed(slot->list->Close(), "Chunk initial Close");
				slot->samplers.push_back(std::make_unique<D3D12GpuSamplerHeap>(device, D3D12Const::kGpuSamplerHeapSize));
				active = slot.get();
				slots.push_back(std::move(slot));
			}
		}
		D3D12_ThrowIfFailed(active->allocator->Reset(), "Chunk allocator Reset");
		D3D12_ThrowIfFailed(active->list->Reset(active->allocator.Get(), nullptr), "Chunk list Reset");
		samplerHeap = 0;
		for (auto &heap : active->samplers)
			heap->Reset();
		ID3D12DescriptorHeap *heaps[] = {views.GetHeap(), active->samplers[samplerHeap]->GetHeap()};
		active->list->SetDescriptorHeaps(2, heaps);
		active->list->SetGraphicsRootSignature(root);
	}
	D3D12_GPU_DESCRIPTOR_HANDLE ViewTable(const D3D12StageTableDesc &desc, D3D12Const::Stage stage)
	{
		using namespace D3D12Const;
		const auto key = desc.TableKey(stage);
		auto &cache = tables[static_cast<uint32>(stage)];
		if (const auto *cached = cache.Find(key, views.GetGeneration(), 1))
			return *cached;
		const auto table = views.AllocateTable(StageTableSize(stage));
		D3D12_CPU_DESCRIPTOR_HANDLE sources[kStageTableSize]{};
		for (uint32 i = RootCBVCount(stage); i < kMaxCBVsPerStage; ++i)
		{
			auto &handle = cbvs[static_cast<uint32>(stage)][i];
			if (!handle.ptr)
				handle = stagingViews.Allocate();
			if (desc.cbvAddress[i])
			{
				D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{desc.cbvAddress[i], AlignUp<uint32>(std::max<uint32>(desc.cbvSize[i], 16), kCBVAlignment)};
				device->CreateConstantBufferView(&cbv, handle);
			}
			sources[i - RootCBVCount(stage)] = desc.cbvAddress[i] ? handle : nullCBV;
		}
		for (uint32 i = 0; i < kMaxSRVsPerStage; ++i)
			sources[StageTableOffsetSRV(stage) + i] = desc.srv[i].ptr ? desc.srv[i] : nullSRV;
		sources[StageTableOffsetUAV(stage)] = nullUAV;
		const UINT size = StageTableSize(stage);
		device->CopyDescriptors(1, &table.cpu, &size, size, sources, nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		cache.Store(key, table.gpu, views.GetGeneration(), 1);
		return table.gpu;
	}
	void Draw(const D3D12ResolvedDraw &draw)
	{
		using namespace D3D12Const;
		auto *list = active->list.Get();
		const auto &packet = draw.source.packet;
		if (draw.deferredIndices)
			draw.deferredIndices->Finish();
		D3D12_GPU_DESCRIPTOR_HANDLE samplerTables[3]{};
		for (unsigned attempt = 0; attempt < 2; ++attempt)
		{
			bool success = true;
			for (uint32 i = 0; i < draw.source.block.stageCount; ++i)
			{
				const auto &source = draw.source.block.stages[i];
				auto &memo = samplerMemo[static_cast<uint32>(source.stage)];
				const auto generation = active->samplers[samplerHeap]->GetGeneration();
				if (const auto *cached = memo.Find(source.layoutIdentity, generation, source.samplers, source.samplerCount))
				{
					samplerTables[i] = *cached;
					continue;
				}
				uint32 ids[kMaxSamplersPerStage]{};
				D3D12_CPU_DESCRIPTOR_HANDLE handles[kMaxSamplersPerStage]{};
				for (uint32 j = 0; j < source.samplerCount; ++j)
					handles[j] = samplers.GetSampler(source.samplers[j], ids[j]);
				if (!active->samplers[samplerHeap]->GetOrCreateTable(ids, handles, source.samplerCount, samplerTables[i]))
				{
					success = false;
					break;
				}
				memo.Store(source.layoutIdentity, generation, source.samplers, source.samplerCount, samplerTables[i]);
			}
			if (success)
				break;
			if (attempt)
				throw std::runtime_error("Latte chunk sampler bound exceeded");
			++samplerHeap;
			samplerMemo = {};
			if (samplerHeap == active->samplers.size())
				active->samplers.push_back(std::make_unique<D3D12GpuSamplerHeap>(device, kGpuSamplerHeapSize));
			ID3D12DescriptorHeap *heaps[] = {views.GetHeap(), active->samplers[samplerHeap]->GetHeap()};
			list->SetDescriptorHeaps(2, heaps);
		}
		if (!draw.barriers.empty())
			list->ResourceBarrier(static_cast<UINT>(draw.barriers.size()), draw.barriers.data());
		list->SetPipelineState(draw.pipeline);
		list->IASetPrimitiveTopology(draw.topology);
		list->OMSetRenderTargets(draw.targetCount, draw.targets.data(), FALSE, draw.depth.ptr ? &draw.depth : nullptr);
		list->IASetVertexBuffers(0, LATTE_MAX_VERTEX_BUFFERS, draw.vertices.data());
		list->RSSetViewports(1, &packet.viewport);
		list->RSSetScissorRects(1, &packet.scissor);
		if (draw.blendConstants)
		{
			float blend[4];
			memcpy(blend, packet.blendFactor, sizeof(blend));
			if (draw.blendAlphaOnly)
				blend[0] = blend[1] = blend[2] = blend[3];
			list->OMSetBlendFactor(blend);
		}
		if (draw.stencil)
			list->OMSetStencilRef(packet.stencilRef);
		for (uint32 i = 0; i < draw.source.block.stageCount; ++i)
		{
			const auto &source = draw.source.block.stages[i];
			auto desc = draw.stages[i];
			for (uint32 reg = 0; reg < kMaxSRVsPerStage; ++reg)
				if (const auto &texture = draw.textures[i][reg]; texture.view)
					desc.srv[reg] = texture.view->GetRecordingSRV(texture.swizzle);
			if (source.uniformReg >= 0)
			{
				const auto &bytes = draw.source.uniforms[i];
				auto &cached = uniforms[static_cast<uint32>(source.stage)];
				if (!cached.address || cached.bytes != bytes)
				{
					const auto size = AlignUp<uint32>(std::max<size_t>(bytes.size(), 16), kCBVAlignment);
					const auto upload = uploads.Allocate(size, kCBVAlignment);
					memcpy(upload.cpuPtr, bytes.data(), bytes.size());
					cached.bytes = bytes;
					cached.address = upload.gpuAddress;
					uniformBytes += size;
				}
				desc.cbvAddress[source.uniformReg] = cached.address;
				desc.cbvSize[source.uniformReg] = static_cast<uint32>(bytes.size());
			}
			list->SetGraphicsRootDescriptorTable(RootParamStageSamplers(source.stage), samplerTables[i]);
			for (uint32 j = 0; j < RootCBVCount(source.stage); ++j)
				list->SetGraphicsRootConstantBufferView(RootParamStageRootCBV(source.stage, j), desc.cbvAddress[j] ? desc.cbvAddress[j] : zero);
			list->SetGraphicsRootDescriptorTable(RootParamStageTable(source.stage), ViewTable(desc, source.stage));
		}
		if (draw.source.runtime.size() != kRuntimeDataDwords * sizeof(uint32))
			throw std::logic_error("Invalid Latte chunk runtime constants");
		list->SetGraphicsRoot32BitConstants(kRootParamRuntimeData, kRuntimeDataDwords, draw.source.runtime.data(), 0);
		if (packet.indexType != 0)
		{
			list->IASetIndexBuffer(&draw.indices);
			list->DrawIndexedInstanced(packet.indexCount, packet.instanceCount, 0, static_cast<INT>(packet.baseVertex), packet.baseInstance);
		}
		else
			list->DrawInstanced(packet.count, packet.instanceCount, packet.baseVertex, packet.baseInstance);
	}
};

D3D12ParallelRecorder::D3D12ParallelRecorder(ID3D12Device *device, ID3D12Fence *fence, ID3D12RootSignature *root,
											 D3D12_CPU_DESCRIPTOR_HANDLE nullCBV, D3D12_CPU_DESCRIPTOR_HANDLE nullSRV, D3D12_CPU_DESCRIPTOR_HANDLE nullUAV, D3D12_GPU_VIRTUAL_ADDRESS zero)
{
	for (auto &worker : m_workers)
		worker = std::make_unique<Worker>(device, fence, root, nullCBV, nullSRV, nullUAV, zero);
}
D3D12ParallelRecorder::~D3D12ParallelRecorder() = default;

LattePipeline::ChunkTiming D3D12ParallelRecorder::Record(std::span<const D3D12ResolvedDraw> draws, uint64 fence, unsigned requested, bool profile)
{
	auto timing = m_pool.Run(draws.size(), requested, [&](unsigned worker, LattePipeline::ChunkRange range) {
		if (range.end - range.begin > LattePipeline::ChunkWorkers::DrawsPerChunk)
			throw std::logic_error("Latte chunk draw bound exceeded");
		auto &native = *m_workers[worker];
		native.Begin(fence);
		for (size_t i = range.begin; i < range.end; ++i)
			native.Draw(draws[i]);
		D3D12_ThrowIfFailed(native.active->list->Close(), "Chunk list Close"); }, profile);
	m_count = timing.count;
	for (unsigned i = 0; i < m_count; ++i)
		timing.uniformUploadBytes += m_workers[i]->uniformBytes;
	return timing;
}
void D3D12ParallelRecorder::AppendLists(std::vector<ID3D12CommandList *> &lists)
{
	for (unsigned i = 0; i < m_count; ++i)
		lists.push_back(m_workers[i]->active->list.Get());
}
void D3D12ParallelRecorder::Submitted(uint64 fence)
{
	for (unsigned i = 0; i < m_count; ++i)
	{
		auto &worker = *m_workers[i];
		worker.uploads.OnSubmit(fence);
		worker.views.OnSubmit(fence);
		worker.active->fence = fence;
	}
	m_count = 0;
}
#endif
