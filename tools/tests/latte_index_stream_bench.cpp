// Repeated draw streams exercise cache capacity and relocated guest addresses.
#include "Cafe/HW/Latte/Core/LatteDrawCache.h"
#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include <array>
#include <chrono>
#include <iostream>

int main()
{
	g_renderer = std::make_unique<Renderer>();
	std::vector<std::array<uint16, 8>> candidates(32768);
	std::array<size_t, 512> occupancy{};
	std::vector<const void *> stream;
	for (auto &candidate : candidates)
	{
		LatteDrawCache::IndexKey key{reinterpret_cast<uintptr_t>(candidate.data()), 8, uint32(LatteIndexType::U16_BE), uint32(LattePrimitiveMode::TRIANGLES), 0};
		auto &used = occupancy[key.Slot(occupancy.size())];
		if (used == 2)
			continue;
		++used;
		candidate.fill(_swapEndianU16(uint16(stream.size() + 1)));
		stream.push_back(candidate.data());
		if (stream.size() == 1024)
			break;
	}
	assert(stream.size() == 1024);
	auto decode = [](const void *data) {
		uint32 maximum, count;
		Renderer::INDEX_TYPE type;
		Renderer::IndexAllocation allocation;
		LatteIndices_decode(data, LatteIndexType::U16_BE, 8, LattePrimitiveMode::TRIANGLES, maximum, type, count, allocation);
		assert(allocation.rendererInternal && count == 8);
	};
	for (auto data : stream)
		decode(data);
	const auto uploads = g_renderer->uploads;
	const auto start = std::chrono::steady_clock::now();
	constexpr size_t frames = 100, draws = 5000;
	for (size_t frame = 0; frame < frames; ++frame)
	{
		for (size_t draw = 0; draw < draws; ++draw)
			decode(stream[draw % stream.size()]);
		LatteIndices_invalidateOnWait();
	}
	const auto misses = g_renderer->uploads - uploads;
	const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / frames;
	const double hits = 100.0 * (frames * draws - misses) / (frames * draws);
	std::cout << "Repeated index stream: " << hits << "% hits; " << misses << "/" << frames * draws << " misses; " << ms << " ms/5000 draws" << std::endl;
	LatteIndices_invalidateAll(g_renderer.get());
	g_renderer.reset();
	return hits >= 99.0 ? 0 : 1;
}
