// Exercise the production transport with the console's index-churn workload.
// Native allocations/fences are modeled; these are not Xbox frame predictions.
#include "Cafe/HW/Latte/Renderer/D3D12/LattePipeline.h"
#include <iostream>

using namespace LattePipeline;
struct IndexRecorder
{
	struct Index
	{
		ResourceId id;
		std::array<std::byte, 1024> native{};
	};
	struct Work
	{
		ResourceId id;
		Span bytes;
		uint32_t action;
	};
	Stream &stream;
	Registry registry{32768};
	uint64_t draws = 0, uploads = 0;
	void Record(const Work &work, const Arena &arena)
	{
		if (work.action == 0)
		{
			registry.Get<Index>(work.id);
			if (stream.ActiveHeader().reserved & 1)
				stream.Result().words[0] = 1;
		}
		else if (work.action == 1)
		{
			auto *index = registry.Get<Index>(work.id);
			const auto bytes = arena.Read(work.bytes);
			assert(bytes.size() == index->native.size());
			memcpy(index->native.data(), bytes.data(), bytes.size());
			++uploads;
		}
		else if (work.action == 2)
		{
			if (work.id)
				assert(registry.Get<Index>(work.id)->native[0] == std::byte{0x35});
			++draws;
		}
		else
		{
			auto *index = registry.Get<Index>(work.id);
			registry.Use(work.id, stream.ActiveHeader().sequence, 1);
			registry.Release(work.id);
			delete index;
			registry.Retire(stream.ActiveHeader().sequence, 1, &stream.RetiredResources());
		}
	}
};

int main(int argc, char **argv)
{
	const bool synchronous = argc > 1 && std::strcmp(argv[1], "sync") == 0;
	setenv("CEMU_LATTE_PIPELINE", "2", 1);
	Context context;
	Stream stream;
	IndexRecorder recorder{stream};
	stream.SetCompletionHandler([&](const Completion &completion) { recorder.registry.Recycle(completion.retiredResources); });
	stream.Start(context, recorder, [] {}, [] {});
	std::array<std::byte, 1024> input;
	input.fill(std::byte{0x35});
	constexpr uint32_t frames = 10, draws = 5000, misses = 1707;
	const auto start = std::chrono::steady_clock::now();
	uint64_t waits = 0;
	auto execute = [&](Opcode opcode, IndexRecorder::Work work, bool reply = false) {
		const auto sequence = stream.Execute<IndexRecorder::Work, &IndexRecorder::Record>(context, opcode, work, recorder, reply);
		if (reply)
		{
			stream.Wait(sequence);
			const auto result = stream.TakeReply(sequence);
			assert(result.sequence == sequence);
			++waits;
		}
	};
	for (uint32_t frame = 0; frame < frames; ++frame)
	{
		for (uint32_t draw = 0; draw < draws; ++draw)
		{
			if (draw >= misses)
			{
				execute(Opcode::Draw, {{}, {}, 2});
				continue;
			}
			auto *index = new IndexRecorder::Index;
			const auto id = recorder.registry.Insert(index);
			index->id = id;
			execute(Opcode::Create, {id, {}, 0}, synchronous);
			Span bytes;
			while (!stream.Staging().Copy(input, bytes))
				stream.WaitCapacity();
			execute(Opcode::IndexUpload, {id, bytes, 1});
			execute(Opcode::Draw, {id, {}, 2});
			execute(Opcode::Release, {id, {}, 3}, synchronous);
		}
		stream.Finish(); // The only semantic synchronization in the async workload.
	}
	stream.Stop(context, recorder);
	const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / frames;
	assert(recorder.draws == frames * draws && recorder.uploads == frames * misses);
	std::cout << (synchronous ? "Synchronous" : "Asynchronous") << " worker index churn: " << ms << " ms/5000 draws; " << waits / frames << " replies/frame; reply wait " << stream.Metrics().replyWaitNs / (1e6 * frames) << " ms/frame; capacity wait " << stream.Metrics().backpressureNs / (1e6 * frames) << " ms/frame\n";
}
