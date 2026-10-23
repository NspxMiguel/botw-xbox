#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DescriptorHeaps.h"
#include <iostream>
struct Tracker : D3D12SubmissionTracker
{
	uint64 completed = 0;
	uint32 waits = 0;
	void WaitForSubmission(uint64 id) override
	{
		completed = id;
		++waits;
	}
	uint64 GetCompletedSubmissionId() override
	{
		return completed;
	}
};
int main()
{
	ID3D12Device device;
	Tracker tracker;
	D3D12StagingDescriptorHeap staging(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, "test");
	const auto sourceEpoch = staging.GetGeneration();
	const auto source = staging.Allocate();
	staging.Free(source);
	const auto recycledSource = staging.Allocate();
	assert(source.ptr == recycledSource.ptr);
	assert(sourceEpoch != staging.GetGeneration());
	{
		D3D12GpuViewHeap heap(&device, &tracker, 16);
		auto epoch = heap.GetGeneration();
		heap.AllocateTable(10);
		heap.OnSubmit(1);
		assert(epoch != heap.GetGeneration());
		epoch = heap.GetGeneration();
		const auto previousLap = heap.AllocateTable(4);
		heap.Retire(1);
		const auto wrapped = heap.AllocateTable(4);
		assert(wrapped.gpu.ptr < previousLap.gpu.ptr);
		assert(epoch != heap.GetGeneration());
		epoch = heap.GetGeneration();
		heap.OnSubmit(2);
		assert(epoch != heap.GetGeneration());
		epoch = heap.GetGeneration();
		heap.Retire(2);
		const auto reset = heap.AllocateTable(4);
		assert(reset.gpu.ptr == wrapped.gpu.ptr);
		assert(epoch != heap.GetGeneration());
	}
	{
		D3D12GpuViewHeap heap(&device, &tracker, 16);
		const auto first = heap.AllocateTable(12);
		heap.OnSubmit(3);
		auto epoch = heap.GetGeneration();
		const auto afterWait = heap.AllocateTable(8);
		assert(tracker.waits == 1);
		assert(tracker.completed == 3);
		assert(first.gpu.ptr == afterWait.gpu.ptr);
		assert(epoch != heap.GetGeneration());
	}
	{
		D3D12GpuSamplerHeap heap(&device, 4);
		uint32 ids[] = {1, 2, 3};
		D3D12_CPU_DESCRIPTOR_HANDLE handles[] = {{1}, {2}, {3}};
		D3D12_GPU_DESCRIPTOR_HANDLE table{}, repeated{}, afterReset{};
		auto epoch = heap.GetGeneration();
		assert(heap.GetOrCreateTable(ids, handles, 3, table));
		const auto copies = device.copies;
		assert(heap.GetOrCreateTable(ids, handles, 3, repeated));
		assert(table.ptr == repeated.ptr && copies == device.copies);
		ids[0] = 4;
		assert(!heap.GetOrCreateTable(ids, handles, 3, repeated));
		assert(epoch == heap.GetGeneration());
		heap.Reset();
		assert(epoch != heap.GetGeneration());
		assert(heap.GetOrCreateTable(ids, handles, 3, afterReset));
		assert(table.ptr == afterReset.ptr);
	}
	std::cout << "Descriptor heap lifetime tests passed (staging reuse, submit, wrap, retirement/wait, sampler reset/cache).\n";
}
