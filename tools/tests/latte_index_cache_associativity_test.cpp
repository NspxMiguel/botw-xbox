#include "Cafe/HW/Latte/Core/LatteDrawCache.h"
#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/HW/Latte/Core/LatteProfiler.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"

#include <array>
#include <iostream>

static uint32
Decode(const void *data, uint32 count = 4,
	   LatteIndexType type = LatteIndexType::U16_BE,
	   LattePrimitiveMode primitive = LattePrimitiveMode::TRIANGLES)
{
	uint32 maximum = 0, outputCount = 0;
	Renderer::INDEX_TYPE outputType;
	Renderer::IndexAllocation allocation{};
	LatteIndices_decode(data, type, count, primitive, maximum, outputType,
						outputCount, allocation);
	return maximum;
}

int main()
{
	g_renderer = std::make_unique<Renderer>();
	// Five complete keys in one set distinguish four-way LRU from a direct map.
	std::vector<std::array<uint16, 4>> candidates(4096);
	std::array<uint16 *, 5> collision{};
	size_t found = 0;
	size_t bucket = 0;
	for (auto &candidate : candidates)
	{
		const LatteDrawCache::IndexKey key{
			reinterpret_cast<uintptr_t>(candidate.data()), 4,
			uint32(LatteIndexType::U16_BE), uint32(LattePrimitiveMode::TRIANGLES),
			UINT32_MAX};
		if (found && key.Slot(64) != bucket)
			continue;
		bucket = key.Slot(64);
		candidate.fill(_swapEndianU16(uint16(found + 1)));
		collision[found++] = candidate.data();
		if (found == collision.size())
			break;
	}
	assert(found == collision.size());
	for (unsigned i = 0; i < 4; ++i)
		assert(Decode(collision[i]) == i + 1);
	assert(g_renderer->live == 4);
	auto uploads = g_renderer->uploads;
	// All ways survive waits and alternating access, with exact values.
	LatteIndices_invalidateOnWait();
	for (unsigned i = 0; i < 4; ++i)
		assert(Decode(collision[i]) == i + 1);
	assert(g_renderer->uploads == uploads);
	// A content mutation replaces its own way, not another key in the set.
	collision[2][1] = _swapEndianU16(101);
	assert(Decode(collision[2]) == 101);
	assert(g_renderer->live == 4 && g_renderer->uploads == ++uploads);
	assert(Decode(collision[0]) == 1); // Refresh way zero; way one is now oldest.
	assert(Decode(collision[4]) == 5);
	assert(g_renderer->live == 4 && g_renderer->uploads == ++uploads);
	assert(Decode(collision[0]) == 1);
	assert(Decode(collision[2]) == 101);
	assert(Decode(collision[3]) == 4);
	assert(g_renderer->uploads == uploads);
	assert(Decode(collision[1]) == 2); // Evicted key must upload again.
	assert(g_renderer->uploads == uploads + 1);
	LatteIndices_invalidateAll(g_renderer.get());
	assert(g_renderer->live == 0);

	// AUTO bytes do not depend on an input address, including a null address.
	assert(Decode(nullptr, 4, LatteIndexType::AUTO, LattePrimitiveMode::QUADS) ==
		   3);
	uploads = g_renderer->uploads;
	assert(Decode(collision[0], 4, LatteIndexType::AUTO,
				  LattePrimitiveMode::QUADS) == 3);
	assert(g_renderer->uploads == uploads);
	LatteIndices_invalidateAll(g_renderer.get());
	assert(g_renderer->live == 0);
	g_renderer.reset();
	std::cout << "Index associativity tests passed (LRU, mutation, waits, "
				 "canonical AUTO key).\n";
}
