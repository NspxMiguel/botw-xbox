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
	using namespace D3D12Const;
	for (auto stage : {Stage::Vertex, Stage::Pixel, Stage::Geometry})
	{
		auto volatileBindings = desc;
		for (uint32 i = 0; i < RootCBVCount(stage); ++i)
		{
			volatileBindings.cbvAddress[i] = 256 * (i + 1);
			volatileBindings.cbvSize[i] = 256;
		}
		assert(volatileBindings.TableKey(stage) == desc.TableKey(stage));
		assert(volatileBindings.TableKey(stage).Hash() == desc.TableKey(stage).Hash());
		assert(!(volatileBindings.TableKey(Stage::Count) == desc.TableKey(Stage::Count)));
		volatileBindings.cbvAddress[RootCBVCount(stage)] = 4096;
		assert(!(volatileBindings.TableKey(stage) == desc.TableKey(stage)));
	}
	D3D12StageTableCache<4> hashCache;
	assert(!hashCache.Find(desc, 1, 1));
	hashCache.Store(desc, {77}, 1, 1);
	assert(hashCache.Find(desc, 1, 1)->ptr == 77);
	assert(!hashCache.Find(desc, 2, 1));
	assert(!hashCache.Find(desc, 1, 2));
	hashCache.Store(desc, {88}, 2, 1);
	assert(!hashCache.Find(desc, 1, 1));
	assert(hashCache.Find(desc, 2, 1)->ptr == 88);
	hashCache.Store(desc, {99}, 2, 2);
	assert(!hashCache.Find(desc, 2, 1));
	assert(hashCache.Find(desc, 2, 2)->ptr == 99);
	hashCache.Clear();
	assert(!hashCache.Find(desc, 2, 2));
	// Force collisions in the production hash and fill the probe chain completely.
	std::array<D3D12StageTableDesc, 5> collisions{};
	uint32 found = 0;
	for (uintptr_t handle = 1; found < collisions.size(); ++handle)
	{
		auto key = desc;
		key.srv[0].ptr = handle;
		if ((key.Hash() & 3) == (desc.Hash() & 3))
			collisions[found++] = key;
	}
	for (uint32 i = 0; i < 4; ++i)
		hashCache.Store(collisions[i], {100 + i}, 3, 4);
	for (uint32 i = 0; i < 4; ++i)
		assert(hashCache.Find(collisions[i], 3, 4)->ptr == 100 + i);
	assert(!hashCache.Find(collisions[4], 3, 4));
	hashCache.Store(collisions[2], {555}, 3, 4);
	assert(hashCache.Find(collisions[2], 3, 4)->ptr == 555);
	hashCache.Store(collisions[4], {999}, 3, 4);
	assert(hashCache.Find(collisions[4], 3, 4)->ptr == 999);
	assert(!hashCache.Find(collisions[0], 3, 4));
	assert(hashCache.Find(collisions[1], 3, 4)->ptr == 101);
	assert(hashCache.Find(collisions[2], 3, 4)->ptr == 555);
	assert(hashCache.Find(collisions[3], 3, 4)->ptr == 103);
	assert(!hashCache.Find(desc, 3, 4));
	// The default cache caps long probe chains even before all slots are full.
	D3D12StageTableCache<32> bounded;
	std::array<D3D12StageTableDesc, 17> chain{};
	found = 0;
	for (uintptr_t handle = 1; found < chain.size(); ++handle)
	{
		auto key = desc;
		key.srv[0].ptr = handle;
		if ((key.Hash() & 31) == (desc.Hash() & 31))
			chain[found++] = key;
	}
	for (uint32 i = 0; i < chain.size(); ++i)
		bounded.Store(chain[i], {200 + i}, 1, 1);
	assert(!bounded.Find(chain[0], 1, 1));
	for (uint32 i = 1; i < chain.size(); ++i)
		assert(bounded.Find(chain[i], 1, 1)->ptr == 200 + i);
	std::cout << "Production stage table tests passed (all fields, epochs, reset).\n";
}
