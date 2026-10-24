#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/HW/Latte/Core/LatteDrawCache.h"
#include "Cafe/HW/Latte/Core/LatteContentIndex.h"
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

template <typename T>
static void CheckConversionParity()
{
	for (uint32 count = 4; count <= 67; ++count)
	{
		std::vector<T> values(count), input(count);
		for (uint32 i = 0; i < count; ++i)
		{
			values[i] = T((i * 0x1937542bu) ^ (count * 0x12345u));
			if (values[i] == std::numeric_limits<T>::max())
				--values[i];
			if constexpr (sizeof(T) == 2)
				input[i] = _swapEndianU16(values[i]);
			else
				input[i] = _swapEndianU32(values[i]);
		}
		for (auto mode : {LattePrimitiveMode::TRIANGLES, LattePrimitiveMode::QUADS, LattePrimitiveMode::QUAD_STRIP, LattePrimitiveMode::LINE_LOOP, LattePrimitiveMode::TRIANGLE_FAN})
		{
			std::vector<T> expected;
			if (mode == LattePrimitiveMode::QUADS || mode == LattePrimitiveMode::QUAD_STRIP)
			{
				const bool strip = mode == LattePrimitiveMode::QUAD_STRIP;
				for (uint32 i = 0; i + 3 < count; i += strip ? 2 : 4)
				{
					const uint32 order[] = {0, 1, 2, strip ? 2u : 0u, strip ? 1u : 2u, 3};
					for (auto vertex : order)
						expected.push_back(values[i + vertex]);
				}
			}
			else if (mode == LattePrimitiveMode::TRIANGLE_FAN)
			{
				for (uint32 i = 0; i < count; ++i)
					expected.push_back(values[i % 2 ? count - 1 - i / 2 : i / 2]);
			}
			else
			{
				expected = values;
				if (mode == LattePrimitiveMode::LINE_LOOP)
					expected.push_back(values[0]);
			}
			const auto result = Decode(input.data(), count, sizeof(T) == 2 ? LatteIndexType::U16_BE : LatteIndexType::U32_BE, mode);
			assert(result.count == expected.size());
			assert(result.max == *std::max_element(expected.begin(), expected.end()));
			assert(std::memcmp(result.allocation.mem, expected.data(), expected.size() * sizeof(T)) == 0);
			// A changed middle value is revalidated even with the same address/count.
			LatteIndices_invalidateAll(g_renderer.get());
		}
		const auto little = Decode(values.data(), count, sizeof(T) == 2 ? LatteIndexType::U16_LE : LatteIndexType::U32_LE);
		assert(little.max == *std::max_element(values.begin(), values.end()));
		assert(std::memcmp(little.allocation.mem, values.data(), count * sizeof(T)) == 0);
		LatteIndices_invalidateAll(g_renderer.get());
	}
}

int main()
{
	g_renderer = std::make_unique<Renderer>();
	LatteProfiler::s_state.enabled = true;
	CheckConversionParity<uint16>();
	CheckConversionParity<uint32>();
	std::array<uint16, 3> restartFirst{_swapEndianU16(0xFFFF), _swapEndianU16(2), _swapEndianU16(4)};
	LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.value = 0xFFFF;
	assert(Decode(restartFirst.data(), restartFirst.size()).max == 4);
	LatteIndices_invalidateAll(g_renderer.get());
	LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.value = UINT32_MAX;

	std::array<uint16, 512> indices{};
	for (size_t i = 0; i < indices.size(); ++i)
		indices[i] = _swapEndianU16(uint16(i));
	auto result = Decode(indices.data(), indices.size());
	assert(result.max == 511 && result.count == 512 && result.type == Renderer::INDEX_TYPE::U16);
	assert(static_cast<uint16 *>(result.allocation.mem)[511] == 511);
	const auto first = result.allocation.rendererInternal;
	const auto uploads = g_renderer->uploads;
	const auto releases = g_renderer->releases;
	assert(Decode(indices.data(), indices.size()).allocation.rendererInternal == first);
	assert(g_renderer->uploads == uploads);
	LatteIndices_invalidateOnWait();
	assert(Decode(indices.data(), indices.size()).allocation.rendererInternal == first);
	assert(g_renderer->uploads == uploads); // small D3D12 entries survive guest waits

	indices[256] = _swapEndianU16(1000); // first/last 64 bytes are unchanged
	result = Decode(indices.data(), indices.size());
	assert(result.max == 1000 && g_renderer->uploads == uploads + 1);
	assert(static_cast<uint16 *>(result.allocation.mem)[256] == 1000);
	assert(g_renderer->releases == releases + 1);

	LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.value = 1000;
	result = Decode(indices.data(), indices.size());
	assert(result.max == 511 && g_renderer->uploads == uploads + 1);
	LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.value = UINT32_MAX;
	assert(Decode(indices.data(), indices.size()).max == 1000);
	assert(g_renderer->uploads == uploads + 1); // Bound metadata changes, output does not.
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
	// Fill all eight transient slots with previous values of one guest address.
	// Every candidate can reject on the next mutation without overflowing the
	// per-lookup duplicate-rejection list.
	for (uint16 value = 4; value < 20; ++value)
	{
		large[100] = _swapEndianU16(value);
		Decode(large.data(), large.size());
	}
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

	// Four colliding keys coexist; the fifth evicts the least recently used.
	std::vector<std::array<uint16, 4>> candidates(16384);
	const LatteDrawCache::IndexKey key{reinterpret_cast<uintptr_t>(indices.data()), 4, uint32(LatteIndexType::U16_BE), uint32(LattePrimitiveMode::TRIANGLES), UINT32_MAX};
	std::vector<const void *> colliding;
	for (auto &candidate : candidates)
	{
		auto other = key;
		other.address = reinterpret_cast<uintptr_t>(candidate.data());
		if (other.Slot(LatteDrawCache::kSmallIndexEntries / LatteDrawCache::kIndexCacheWays) == key.Slot(LatteDrawCache::kSmallIndexEntries / LatteDrawCache::kIndexCacheWays))
		{
			candidate.fill(_swapEndianU16(uint16(7 + colliding.size())));
			colliding.push_back(candidate.data());
			if (colliding.size() == 5)
				break;
		}
	}
	assert(colliding.size() == 5);
	for (size_t i = 0; i < 4; ++i)
		assert(Decode(colliding[i], 4).max == 7 + i);
	assert(g_renderer->live == 4);
	const auto collisionUploads = g_renderer->uploads;
	for (size_t i = 0; i < 4; ++i)
		assert(Decode(colliding[i], 4).max == 7 + i);
	assert(g_renderer->uploads == collisionUploads);
	assert(Decode(colliding[4], 4).max == 11);
	assert(g_renderer->live == 4);
	Decode(colliding[1], 4); // Still present: the oldest was key zero.
	assert(g_renderer->uploads == collisionUploads + 1);
	Decode(colliding[0], 4);
	assert(g_renderer->uploads == collisionUploads + 2);
	assert(LatteProfiler::s_state.counters[size_t(LatteProfiler::Counter::IndexSetEvictions)] >= 2);
	LatteIndices_invalidateAll(g_renderer.get());
	assert(g_renderer->live == 0);

	// Generated indices have no guest address. Null and different caller pointers
	// must share a valid entry, and survive a wait without a sentinel collision.
	result = Decode(nullptr, 8, LatteIndexType::AUTO, LattePrimitiveMode::QUADS);
	const auto generated = result.allocation.rendererInternal;
	const auto generatedUploads = g_renderer->uploads;
	LatteIndices_invalidateOnWait();
	assert(Decode(indices.data(), 8, LatteIndexType::AUTO, LattePrimitiveMode::QUADS).allocation.rendererInternal == generated);
	assert(g_renderer->uploads == generatedUploads);
	LatteIndices_invalidateAll(g_renderer.get());

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
