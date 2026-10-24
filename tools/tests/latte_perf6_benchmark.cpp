// Host CPU probes; these timings do not predict Xbox driver or frame time.
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12StageTable.h"
#include "Cafe/HW/Latte/Core/LatteDrawCache.h"
#include <chrono>
#include <iostream>

using Clock = std::chrono::steady_clock;
template <class Work>
static void Measure(const char *name, size_t calls, Work work)
{
	const auto start = Clock::now();
	uint64_t result = 0;
	for (size_t i = 0; i < calls; ++i)
		result += work(i);
	asm volatile("" : : "r"(result) : "memory");
	std::cout << name << ": " << std::chrono::duration<double, std::nano>(Clock::now() - start).count() / calls << " ns/call\n";
}
// Keep both probes opaque to the caller so a known input cannot eliminate
// the exact comparison or turn either path into a constant-table load.
__attribute__((noinline)) static uint64_t SamplerProbe(D3D12SamplerTableMemo &memo, uint64_t identity, const D3D12_SAMPLER_DESC *values, uint32_t count)
{
	const auto *table = memo.Find(identity, 1, values, count);
	return table ? table->ptr : 0;
}
int main()
{
	std::array<D3D12StageTableDesc, 512> keys{};
	D3D12StageTableCache<> cache;
	for (size_t i = 0; i < keys.size(); ++i)
	{
		for (size_t j = 0; j < D3D12Const::kMaxSRVsPerStage; ++j)
			keys[i].srv[j].ptr = 4096 + i * 64 + j * 32;
		cache.Store(keys[i], {i + 1}, 1, 1);
	}
	Measure("Stage table hash", 1000000, [&](size_t i) { return keys[i % keys.size()].Hash(); });
	Measure("Stage table lookup", 1000000, [&](size_t i) { const auto *p = cache.Find(keys[i % keys.size()], 1, 1); return p ? p->ptr : 0; });
	std::array<uint32_t, 16> output{};
	std::array<uint32_t, 16> packet{};
	for (size_t i = 0; i < packet.size(); ++i)
		packet[i] = uint32_t(i * 13);
	Measure("PM4 short register packet", 2000000, [&](size_t i) {
		asm volatile("" : : "r"(output.data()), "r"(packet.data()) : "memory");
		return LatteDrawCache::StoreBigEndianWords<true>(output.data(), packet.data(), 2 + i % 3);
	});
	D3D12SamplerTableMemo samplerMemo;
	std::array<D3D12_SAMPLER_DESC, 8> samplerValues{};
	samplerMemo.Store(1024, 1, samplerValues.data(), samplerValues.size(), {123});
	Measure("Exact sampler memo", 1000000, [&](size_t) {
		asm volatile("" : : "r"(samplerValues.data()) : "memory");
		return SamplerProbe(samplerMemo, 0, samplerValues.data(), samplerValues.size());
	});
	Measure("Immutable sampler memo", 1000000, [&](size_t) {
		asm volatile("" : : "r"(samplerValues.data()) : "memory");
		return SamplerProbe(samplerMemo, 1024, samplerValues.data(), samplerValues.size());
	});
	LatteDrawCache::ContentSnapshot snapshot;
	std::array<uint8_t, 65536> indices{};
	snapshot.Assign(indices.data(), indices.size());
	indices.back() = 1;
	Measure("Changed index tail rejection", 100000, [&](size_t) {
		asm volatile("" : : "r"(indices.data()) : "memory");
		return snapshot.Matches(indices.data(), indices.size());
	});
}
