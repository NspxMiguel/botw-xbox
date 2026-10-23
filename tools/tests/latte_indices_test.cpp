#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/HW/Latte/Core/LatteDrawCache.h"
#include "Cafe/HW/Latte/Core/LatteProfiler.h"

#include <array>
#include <iostream>

struct Result
{
	uint32 max = 0;
	Renderer::INDEX_TYPE type{};
	uint32 count = 0;
	Renderer::IndexAllocation allocation{};
};

static Result Decode(const void *data, uint32 count, LatteIndexType type = LatteIndexType::U16_BE, LattePrimitiveMode primitive = LattePrimitiveMode::TRIANGLES)
{
	Result result;
	LatteIndices_decode(data, type, count, primitive, result.max, result.type, result.count, result.allocation);
	return result;
}

int main()
{
	g_renderer = std::make_unique<Renderer>();
	LatteProfiler::s_state.enabled = true;
	std::array<uint16, 512> indices{};
	for (size_t i = 0; i < indices.size(); ++i)
		indices[i] = _swapEndianU16(uint16(i));
	auto result = Decode(indices.data(), indices.size());
	assert(result.max == 511 && result.count == 512 && result.type == Renderer::INDEX_TYPE::U16);
	assert(static_cast<uint16 *>(result.allocation.mem)[511] == 511);
	const auto first = result.allocation.rendererInternal;
	const auto uploads = g_renderer->uploads;
	assert(Decode(indices.data(), indices.size()).allocation.rendererInternal == first);
	assert(g_renderer->uploads == uploads);
	LatteIndices_invalidateOnWait();
	assert(Decode(indices.data(), indices.size()).allocation.rendererInternal == first);
	assert(g_renderer->uploads == uploads); // small D3D12 entries survive guest waits

	indices[256] = _swapEndianU16(1000); // first/last 64 bytes are unchanged
	result = Decode(indices.data(), indices.size());
	assert(result.max == 1000 && g_renderer->uploads == uploads + 1);
	assert(static_cast<uint16 *>(result.allocation.mem)[256] == 1000);
	assert(g_renderer->releases == 1);

	LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.value = 1000;
	result = Decode(indices.data(), indices.size());
	assert(result.max == 511 && g_renderer->uploads == uploads + 2);
	LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.value = UINT32_MAX;
	// A partial invalidation starts before the cache entry and overlaps its first word.
	LatteIndices_invalidate(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(indices.data()) - 1), 2);
	assert(g_renderer->live == 0);
	Decode(indices.data(), indices.size());
	LatteIndices_invalidateAll(g_renderer.get());
	assert(g_renderer->live == 0);

	// Large inputs use the transient bank and are released on a guest wait.
	std::vector<uint16> large(40000, _swapEndianU16(3));
	Decode(large.data(), large.size());
	assert(g_renderer->live == 1);
	LatteIndices_invalidateOnWait();
	assert(g_renderer->live == 0);
	// Large generated outputs are transient too, despite having no input bytes.
	Decode(indices.data(), 100000, LatteIndexType::AUTO, LattePrimitiveMode::QUADS);
	assert(g_renderer->live == 1);
	LatteIndices_invalidateOnWait();
	assert(g_renderer->live == 0);

	result = Decode(indices.data(), 4, LatteIndexType::AUTO, LattePrimitiveMode::QUADS);
	assert(result.max == 3 && result.count == 6);
	const auto autoUploads = g_renderer->uploads;
	LatteIndices_invalidateOnWait();
	Decode(indices.data(), 4, LatteIndexType::AUTO, LattePrimitiveMode::QUADS);
	assert(g_renderer->uploads == autoUploads);
	LatteIndices_invalidateAll(g_renderer.get());

	// Force a direct-map collision and verify replacement cannot reuse the wrong data.
	std::vector<std::array<uint16, 4>> candidates(512);
	const LatteDrawCache::IndexKey key{reinterpret_cast<uintptr_t>(indices.data()), 4, uint32(LatteIndexType::U16_BE), uint32(LattePrimitiveMode::TRIANGLES), UINT32_MAX};
	Decode(indices.data(), 4);
	bool replaced = false;
	for (auto &candidate : candidates)
	{
		auto other = key;
		other.address = reinterpret_cast<uintptr_t>(candidate.data());
		if (other.Slot(64) != key.Slot(64))
			continue;
		candidate.fill(_swapEndianU16(7));
		assert(Decode(candidate.data(), 4).max == 7);
		assert(g_renderer->live == 1);
		assert(Decode(indices.data(), 4).max == 3);
		replaced = true;
		break;
	}
	assert(replaced);
	LatteIndices_invalidateAll(g_renderer.get());
	assert(g_renderer->live == 0);

	const auto noIndexMisses = LatteProfiler::s_state.counters[size_t(LatteProfiler::Counter::IndexMisses)];
	result = Decode(nullptr, 6, LatteIndexType::AUTO);
	assert(result.type == Renderer::INDEX_TYPE::NONE && result.count == 6 && !result.allocation.rendererInternal);
	assert(LatteProfiler::s_state.counters[size_t(LatteProfiler::Counter::IndexMisses)] == noIndexMisses);

	// unique_ptr::reset can clear the global before invoking the old renderer destructor.
	Decode(indices.data(), 4);
	auto owner = std::move(g_renderer);
	LatteIndices_invalidateAll(owner.get());
	assert(owner->live == 0);
	g_renderer = std::move(owner);

	// Other backends keep their previous allocation invalidation boundary.
	g_renderer->api = RendererAPI::Vulkan;
	Decode(indices.data(), 4);
	LatteIndices_invalidateOnWait();
	assert(g_renderer->live == 0);
	assert(LatteProfiler::s_state.counters[size_t(LatteProfiler::Counter::IndexHits)] >= 3);
	assert(LatteProfiler::s_state.counters[size_t(LatteProfiler::Counter::IndexMisses)] >= 8);
	g_renderer.reset();
	std::cout << "Index cache integration tests passed (content, restart, waits, collision, bounded retention, release).\n";
}
