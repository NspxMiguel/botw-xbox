#include "Cafe/HW/Latte/Core/LatteContentIndex.h"
#include "Cafe/HW/Latte/Core/LatteDrawCache.h"
#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/HW/Latte/Core/LatteProfiler.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"

#include <iostream>

static Renderer::IndexAllocation Decode(const void *data, uint32 count = 8)
{
	uint32 maximum, outputCount;
	Renderer::INDEX_TYPE outputType;
	Renderer::IndexAllocation allocation;
	LatteIndices_decode(data, LatteIndexType::U16_BE, count, LattePrimitiveMode::TRIANGLES, maximum, outputType, outputCount, allocation);
	return allocation;
}

int main()
{
	g_renderer = std::make_unique<Renderer>();
	LatteProfiler::s_state.enabled = true;
	LatteProfiler::s_state.last = LatteProfiler::Clock::now();
	std::array<uint16, 8> first{}, second{};
	first.fill(_swapEndianU16(123));
	second = first;
	const auto original = Decode(first.data()).rendererInternal;
	auto uploads = g_renderer->uploads;
	assert(Decode(second.data()).rendererInternal == original);
	assert(g_renderer->uploads == uploads);
	assert(LatteProfiler::s_state.counters[size_t(LatteProfiler::Counter::IndexRelocatedHits)] == 1);
	LatteIndices_invalidateOnWait();
	assert(Decode(second.data()).rendererInternal == original);
	second[3] = _swapEndianU16(321);
	const auto modified = Decode(second.data());
	assert(g_renderer->uploads == ++uploads);
	assert(static_cast<uint16 *>(modified.mem)[3] == 321);
	assert(Decode(first.data()).rendererInternal == original);
	// Invalidating the original allocation leaves a stale content hint. It must
	// not return freed memory when the relocated bytes are looked up again.
	LatteIndices_invalidate(first.data(), sizeof(first));
	second = first;
	Decode(second.data());
	assert(g_renderer->uploads == ++uploads);
	LatteIndices_invalidateAll(g_renderer.get());

	// Deliberately collide the complete fingerprint. Byte validation still wins.
	std::array<uint64_t, 2> a{}, b{};
	constexpr uint64_t multiplier = 0x9E3779B185EBCA87ull;
	const uint64_t seed = 8ull ^ (uint64_t(LatteIndexType::U16_BE) << 32) ^ (uint64_t(LattePrimitiveMode::TRIANGLES) << 48);
	b[0] = 1;
	b[1] = ((seed ^ 1) * multiplier) ^ (seed * multiplier);
	assert(LatteDrawCache::IndexContentHash(a.data(), sizeof(a), 8, uint32(LatteIndexType::U16_BE), uint32(LattePrimitiveMode::TRIANGLES)) ==
		   LatteDrawCache::IndexContentHash(b.data(), sizeof(b), 8, uint32(LatteIndexType::U16_BE), uint32(LattePrimitiveMode::TRIANGLES)));
	Decode(a.data());
	uploads = g_renderer->uploads;
	const auto collision = Decode(b.data());
	assert(g_renderer->uploads == uploads + 1);
	for (size_t i = 0; i < 8; ++i)
	{
		uint16 expected;
		memcpy(&expected, reinterpret_cast<const uint8 *>(b.data()) + i * 2, 2);
		assert(static_cast<uint16 *>(collision.mem)[i] == _swapEndianU16(expected));
	}
	LatteIndices_invalidateAll(g_renderer.get());

	// Repeated relocated large buffers skip fingerprinting after the first hit.
	// Samples only select candidates: changing an unsampled byte must miss.
	std::vector<uint16> largeA(32768, _swapEndianU16(77)), largeB = largeA;
	const auto largeOriginal = Decode(largeA.data(), uint32(largeA.size())).rendererInternal;
	assert(Decode(largeB.data(), uint32(largeB.size())).rendererInternal == largeOriginal);
	const auto hashedBefore = LatteProfiler::s_state.counters[size_t(LatteProfiler::Counter::IndexHashedBytes)];
	for (uint32 i = 0; i < 100; ++i)
		assert(Decode(largeB.data(), uint32(largeB.size())).rendererInternal == largeOriginal);
	assert(LatteProfiler::s_state.counters[size_t(LatteProfiler::Counter::IndexHashedBytes)] == hashedBefore);
	const auto fingerprint = LatteDrawCache::IndexContentHash(largeB.data(), largeB.size() * 2, uint32(largeB.size()), uint32(LatteIndexType::U16_BE), uint32(LattePrimitiveMode::TRIANGLES));
	largeB[8191] = _swapEndianU16(321);
	assert(fingerprint == LatteDrawCache::IndexContentHash(largeB.data(), largeB.size() * 2, uint32(largeB.size()), uint32(LatteIndexType::U16_BE), uint32(LattePrimitiveMode::TRIANGLES)));
	uploads = g_renderer->uploads;
	const auto largeModified = Decode(largeB.data(), uint32(largeB.size()));
	assert(g_renderer->uploads == uploads + 1);
	assert(static_cast<uint16 *>(largeModified.mem)[8191] == 321);
	assert(Decode(largeA.data(), uint32(largeA.size())).rendererInternal == largeOriginal);
	LatteIndices_invalidate(largeA.data(), uint32(largeA.size() * 2));
	largeB = largeA;
	uploads = g_renderer->uploads;
	Decode(largeB.data(), uint32(largeB.size()));
	assert(g_renderer->uploads == uploads + 1);
	LatteIndices_invalidateAll(g_renderer.get());

	// A thousand distinct small streams survive together. Keep each set below
	// associativity to test capacity without depending on allocator addresses.
	std::vector<std::array<uint16, 8>> candidates(32768);
	std::array<size_t, LatteDrawCache::kSmallIndexEntries / LatteDrawCache::kIndexCacheWays> occupancy{};
	std::vector<const void *> selected;
	for (auto &candidate : candidates)
	{
		LatteDrawCache::IndexKey key{reinterpret_cast<uintptr_t>(candidate.data()), 8, uint32(LatteIndexType::U16_BE), uint32(LattePrimitiveMode::TRIANGLES), 0};
		auto &used = occupancy[key.Slot(occupancy.size())];
		if (used == 2)
			continue;
		++used;
		candidate.fill(_swapEndianU16(uint16(selected.size() + 1)));
		selected.push_back(candidate.data());
		Decode(candidate.data());
		if (selected.size() == 1024)
			break;
	}
	assert(selected.size() == 1024);
	uploads = g_renderer->uploads;
	LatteIndices_invalidateOnWait();
	for (const auto *input : selected)
		Decode(input);
	assert(g_renderer->uploads == uploads);
	LatteIndices_invalidateAll(g_renderer.get());
	assert(g_renderer->live == 0);
	g_renderer.reset();
	std::cout << "Index relocation tests passed (mutation, stale hint, exact hash collision, small-bank capacity).\n";
}
