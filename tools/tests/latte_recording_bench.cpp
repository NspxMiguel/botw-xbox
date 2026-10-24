// Production registry retirement with a large live scene and sparse releases.
// Host wall times describe CPU bookkeeping, never Xbox driver or frame times.
#include "Cafe/HW/Latte/Renderer/D3D12/LattePipeline.h"
#include <chrono>
#include <iostream>

int main()
{
	using namespace LattePipeline;
	Registry registry{65536};
	std::vector<uint64_t> objects(60000, 1);
	std::vector<ResourceId> ids;
	for (auto &object : objects)
		ids.push_back(registry.Insert(&object));
	constexpr uint32_t frames = 1000, released = 300, submissions = 7;
	const auto start = std::chrono::steady_clock::now();
	for (uint32_t frame = 0; frame < frames; ++frame)
	{
		for (uint32_t i = 0; i < released; ++i)
		{
			registry.Use(ids[i], frame + 1, frame + 1);
			registry.Release(ids[i]);
		}
		for (uint32_t submission = 0; submission < submissions; ++submission)
			registry.Retire(frame + 1, frame + 1);
		for (uint32_t i = 0; i < released; ++i)
			ids[i] = registry.Insert(&objects[i]);
	}
	const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / frames;
	for (auto id : ids)
		assert(*registry.Get<uint64_t>(id) == 1);
	std::cout << "Sparse registry retirement: " << ms << " ms/frame; 60000 live resources, 300 releases, 7 retirements\n";
}
