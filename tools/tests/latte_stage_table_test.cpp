#include "Cafe/HW/Latte/Renderer/D3D12/D3D12StageTable.h"
#include "Cafe/HW/Latte/Core/LatteDrawCache.h"
#include <iostream>

int main()
{
	LatteDrawCache::ExactCache<D3D12StageTableDesc, D3D12_GPU_DESCRIPTOR_HANDLE> cache;
	D3D12StageTableDesc desc{};
	cache.Store(desc, {12}, 1, 1);
	assert(cache.Find(desc, 1, 1)->ptr == 12);
	for (uint32 i = 0; i < D3D12Const::kMaxCBVsPerStage; ++i)
	{
		auto changed = desc;
		changed.cbvAddress[i] = 256;
		assert(!cache.Find(changed, 1, 1));
		changed = desc;
		changed.cbvSize[i] = 256;
		assert(!cache.Find(changed, 1, 1));
	}
	for (uint32 i = 0; i < D3D12Const::kMaxSRVsPerStage; ++i)
	{
		auto changed = desc;
		changed.srv[i].ptr = 42;
		assert(!cache.Find(changed, 1, 1));
	}
	auto changed = desc;
	changed.uavResource = reinterpret_cast<ID3D12Resource *>(uintptr_t(1));
	assert(!cache.Find(changed, 1, 1));
	changed = desc;
	changed.uavSize = 4096;
	assert(!cache.Find(changed, 1, 1));
	assert(!cache.Find(desc, 2, 1)); // Upload-ring wrap/submit.
	assert(!cache.Find(desc, 1, 2)); // Freed SRV slot reused for another view.
	cache.Clear();					 // Resource recreation, internal pass or explicit reset.
	assert(!cache.Find(desc, 1, 1));
	std::cout << "Production stage table tests passed (all fields, epochs, reset).\n";
}
