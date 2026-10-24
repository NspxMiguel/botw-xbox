#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
namespace Latte
{
enum class E_DIM : uint32_t
{
	DIM_2D
};
} // namespace Latte
#define LATTE_MAX_VERTEX_BUFFERS 16
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DrawPacket.h"

#include <chrono>
#include <ctime>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <iomanip>
#include <iostream>

using namespace LattePipeline;
using Clock = std::chrono::steady_clock;

struct Recorder
{
	uint64_t commands = 0, sequence = 0, pinnedFence = 0;
	BlockId pinnedBlock;
	Registry resources;
	std::array<uint64_t, 64> objects{};
	std::array<ResourceId, 64> ids{};
	Recorder()
	{
		for (uint32_t i = 0; i < objects.size(); ++i)
		{
			objects[i] = i * 13;
			ids[i] = resources.Insert(&objects[i]);
		}
	}
	StateBlocks<DrawBlock, 4, DrawBlockEqual> blocks;
	std::array<std::byte, sizeof(DrawBlock)> state{};
	std::array<std::byte, 1024> lastUniform{};
	__attribute__((noinline)) void Commands(const DrawPacket &packet, const DrawBlock &block, std::span<const std::byte> uniform, bool native = false)
	{
		// Stand in for native state/descriptor equality checks; no GPU or driver
		// work is claimed by this host-only benchmark.
		commands += packet.count + packet.baseVertex + block.stageCount;
		for (auto id : {packet.pipeline, packet.framebuffer, packet.indices})
			commands += native ? *resources.Get<uint64_t>(id) : objects[id.slot];
		for (uint32_t i = 0; i < block.stageCount; ++i)
			for (uint32_t j = 0; j < block.stages[i].textureCount; ++j)
			{
				auto id = block.stages[i].textures[j].view;
				commands += native ? *resources.Get<uint64_t>(id) : objects[id.slot];
				if (!native)
					commands += objects[id.slot];
			}
		commands += std::memcmp(state.data(), &block, sizeof(block)) != 0;
		commands += std::memcmp(lastUniform.data(), uniform.data(), uniform.size()) != 0;
		std::memcpy(state.data(), &block, sizeof(block));
		std::memcpy(lastUniform.data(), uniform.data(), uniform.size());
		asm volatile("" : : "r"(commands) : "memory");
	}
	void Record(const DrawPacket &packet, const Arena &arena)
	{
		const auto &block = blocks.Get(packet.state);
		Commands(packet, block, arena.Read(packet.uniformBytes[0]), true);
		++sequence;
		const uint64_t fence = sequence / 2000 + 1;
		for (auto id : {packet.pipeline, packet.framebuffer, packet.indices})
			resources.Use(id, sequence, fence);
		for (uint32_t i = 0; i < block.stageCount; ++i)
			for (uint32_t j = 0; j < block.stages[i].textureCount; ++j)
			{
				auto id = block.stages[i].textures[j].view;
				commands += *resources.Get<uint64_t>(id);
				if (packet.state != pinnedBlock || fence != pinnedFence)
				{
					resources.Use(id, sequence, fence);
					resources.Use(ids[id.slot + 24], sequence, fence);
				}
			}
		pinnedBlock = packet.state;
		pinnedFence = fence;
	}
};

struct Timing
{
	double wall, cpu;
};
static double ThreadMilliseconds()
{
	timespec time{};
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0)
		std::abort();
	return static_cast<double>(time.tv_sec) * 1000.0 + static_cast<double>(time.tv_nsec) / 1e6;
}
static Timing Run(bool queued, bool churn, uint64_t &commands)
{
	setenv("CEMU_LATTE_PIPELINE", queued ? "1" : "0", 1);
	Context context;
	Stream stream(1024, 16 * 1024 * 1024);
	Recorder recorder;
	DrawBlock block{};
	block.stageCount = 3;
	for (uint32_t i = 0; i < 3; ++i)
	{
		auto &stage = block.stages[i];
		stage.samplerCount = 8;
		stage.textureCount = 8;
		for (uint32_t j = 0; j < 8; ++j)
			stage.textures[j].view = recorder.ids[3 + i * 8 + j];
	}
	std::array<std::byte, 1024> uniform{};
	DrawPacket packet{};
	packet.count = 96;
	packet.instanceCount = 1;
	packet.pipeline = recorder.ids[0];
	packet.framebuffer = recorder.ids[1];
	packet.indices = recorder.ids[2];
	constexpr uint32_t warmupFrames = 5, frames = 20, draws = 5000;
	Clock::time_point start;
	double cpuStart = 0;
	for (uint32_t frame = 0; frame < warmupFrames + frames; ++frame)
	{
		if (frame == warmupFrames)
		{
			// Touch the complete arena/ring and train the caches before the fixed
			// Plateau-style steady-state measurement, rather than timing page faults.
			start = Clock::now();
			cpuStart = ThreadMilliseconds();
		}
		for (uint32_t draw = 0; draw < draws; ++draw)
		{
			packet.baseVertex = draw;
			uniform[0] = static_cast<std::byte>(draw);
			packet.stencilRef = draw & 1;
			if (churn)
				block.stages[0].textures[0].swizzle = draw;
			if (queued)
			{
				std::array<std::byte, 64> runtime{};
				if (!stream.Staging().Copy(runtime, packet.runtime))
					std::abort();
				if (!stream.Staging().Copy(uniform, packet.uniformBytes[0]))
					std::abort();
				packet.state = recorder.blocks.Intern(block, context.Sequence() + 1, stream.Progress().recorded);
				if (!packet.state)
					std::abort();
				stream.Execute<DrawPacket, &Recorder::Record>(context, Opcode::Draw, packet, recorder);
			}
			else
			{
				context.BeginOperation();
				// Reference path records directly from prepared CPU values, with
				// no packet encoding, staging copy or queue cursor traffic.
				recorder.Commands(packet, block, uniform);
			}
		}
		context.EndFrame();
	}
	commands = recorder.commands;
	return {std::chrono::duration<double, std::milli>(Clock::now() - start).count() / frames, (ThreadMilliseconds() - cpuStart) / frames};
}

// Exercise the production sequence memo with synthetic sampler translation.
// This isolates construction/copy work and does not time the D3D12 driver.
__attribute__((noinline)) static void BuildLayout(StageBlock &block, uint32_t seed)
{
	block = {};
	block.stage = D3D12Const::Stage::Vertex;
	block.textureCount = block.samplerCount = 8;
	for (uint32_t unit = 0; unit < 8; ++unit)
	{
		block.textures[unit].view = {unit + 1, 1};
		block.textures[unit].reg = unit;
		block.textures[unit].swizzle = seed;
		auto &sampler = block.samplers[unit];
		sampler.words[0] = 0;
		sampler.words[1] = sampler.words[2] = sampler.words[3] = 1;
		sampler.words[5] = 1;
		sampler.words[11] = std::bit_cast<uint32_t>(float(seed & 63) / 64);
		sampler.words[12] = std::bit_cast<uint32_t>(float((seed >> 6) & 1023) / 64);
	}
}
static void LayoutBenchmark()
{
	std::array<double, 2> time{};
	std::array<uint64_t, 2> checksum{};
	constexpr uint32_t frames = 200, draws = 5000;
	for (const bool cached : {false, true})
	{
		SequenceStageLayouts layouts;
		const auto start = Clock::now();
		for (uint32_t frame = 0; frame < frames; ++frame)
			for (uint32_t draw = 0; draw < draws; ++draw)
			{
				if (draw % 4 == 0)
					layouts.BeginSequence();
				const uint32_t seed = draw / 4;
				StageBlock block;
				if (cached)
					block = layouts.Get(D3D12Const::Stage::Vertex, 1, [&](StageBlock &out) { BuildLayout(out, seed); });
				else
					BuildLayout(block, seed);
				block.uniforms[0] = {draw * 256, 512};
				checksum[cached] += block.textures[0].swizzle + block.uniforms[0].offset;
				asm volatile("" : : "r"(&block) : "memory");
			}
		time[cached] = std::chrono::duration<double, std::milli>(Clock::now() - start).count() / frames;
	}
	assert(checksum[0] == checksum[1]);
	std::cout << "Synthetic stage translation, production sequence memo: rebuild " << time[0] << " ms/5000 stages; cached " << time[1] << " ms/5000 stages; identical dynamic bindings\n";
}

int main()
{
#ifdef __APPLE__
	// Keep the rendering-cost model on foreground scheduling policy. This affects
	// only this benchmark thread; no machine-wide priority or affinity changes.
	if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0) != 0)
		return 3;
#endif
	LayoutBenchmark();
	bool passed = true;
	for (const bool churn : {false, true})
	{
		std::vector<double> direct, queued, overhead, cpuOverhead;
		for (uint32_t run = 0; run < 9; ++run)
		{
			uint64_t a = 0, b = 0;
			const auto baseline = Run(false, churn, a), pipeline = Run(true, churn, b);
			if (a != b)
			{
				std::cerr << "Command mismatch\n";
				return 2;
			}
			direct.push_back(baseline.wall);
			queued.push_back(pipeline.wall);
			overhead.push_back(pipeline.wall - baseline.wall);
			cpuOverhead.push_back(pipeline.cpu - baseline.cpu);
		}
		std::sort(direct.begin(), direct.end());
		std::sort(queued.begin(), queued.end());
		std::sort(overhead.begin(), overhead.end());
		std::sort(cpuOverhead.begin(), cpuOverhead.end());
		std::cout << std::fixed << std::setprecision(4)
				  << (churn ? "Changing binding blocks: " : "Repeated binding blocks: ") << "Packet " << sizeof(Packet) << " B; draw payload " << sizeof(DrawPacket) << " B; state block " << sizeof(DrawBlock) << " B\n"
				  << "5000 draws/frame, 1024 uniform bytes/draw, 5 warmup + 9 x 20 measured frames/path\n"
				  << "Direct median " << direct[4] << " ms/frame; queued median " << queued[4] << " ms/frame\n"
				  << "Paired wall overhead median " << overhead[4] << " ms/frame; maximum " << overhead.back() << " ms/frame\n"
				  << "Paired thread-CPU overhead median " << cpuOverhead[4] << " ms/frame; maximum " << cpuOverhead.back() << " ms/frame\n";
		passed = passed && overhead[4] <= 1.0 && cpuOverhead[4] <= 1.0;
	}
	return passed ? 0 : 1;
}
