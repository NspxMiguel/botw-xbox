// Host-only microbenchmarks. These are not Xbox frame-time predictions.
#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/HW/Latte/Core/LatteProfiler.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"

#include <chrono>
#include <iostream>

static void Decode(const void *data, uint32 count)
{
	uint32 maximum, outputCount;
	Renderer::INDEX_TYPE outputType;
	Renderer::IndexAllocation allocation;
	LatteIndices_decode(data, LatteIndexType::U16_BE, count, LattePrimitiveMode::TRIANGLES, maximum, outputType, outputCount, allocation);
}

template <typename Work>
static double NanosecondsPerCall(Work work, size_t iterations)
{
	const auto start = std::chrono::steady_clock::now();
	for (size_t i = 0; i < iterations; ++i)
		work();
	return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / iterations;
}

int main()
{
	g_renderer = std::make_unique<Renderer>();
	for (const uint32 count : {8u, 512u, 32768u})
	{
		std::vector<uint16> input(count, _swapEndianU16(123));
		Decode(input.data(), count);
		const auto uploads = g_renderer->uploads;
		const auto hitNs = NanosecondsPerCall([&] { Decode(input.data(), count); }, 100000);
		assert(g_renderer->uploads == uploads);
		std::cout << "Host exact index hit, " << count * 2 << " input bytes: " << hitNs << " ns/call\n";
		LatteIndices_invalidateAll(g_renderer.get());
	}
	// Shift the guest address every call while keeping complete contents equal.
	for (const uint32 count : {8u, 512u, 32768u})
	{
		std::vector<std::vector<uint16>> relocated(64, std::vector<uint16>(count, _swapEndianU16(123)));
		size_t next = 0;
		const auto uploads = g_renderer->uploads;
		const auto relocatedNs = NanosecondsPerCall([&] {
			Decode(relocated[next++ % relocated.size()].data(), count);
		},
													100000);
		std::cout << "Host relocated " << count * 2 << "-byte indices: " << relocatedNs << " ns/call; uploads " << g_renderer->uploads - uploads << "/100000 calls\n";
		assert(g_renderer->uploads == uploads + 1);
		LatteIndices_invalidateAll(g_renderer.get());
	}
	g_renderer.reset();

	using namespace LatteProfiler;
	s_state.enabled = true;
	s_state.current = Slot::Pm4;
	s_state.last = Clock::now();
	const auto wrappers = NanosecondsPerCall([] {
		Scope draw(Slot::DrawOther);
		Scope state(Slot::DrawStateDecode);
	},
											 100000);
	const auto combined = NanosecondsPerCall([] {
		DrawScope draw(Slot::DrawStateDecode);
	},
											 100000);
	std::cout << "Host empty draw profiling: paired scopes " << wrappers << " ns/call, combined scope " << combined << " ns/call\n";
}
