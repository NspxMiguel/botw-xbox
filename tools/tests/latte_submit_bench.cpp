// Nonblocking submission transport parity; native driver work is not modeled.
#include "Cafe/HW/Latte/Renderer/D3D12/LattePipeline.h"
#include <iostream>
using namespace LattePipeline;
struct SubmitRecorder
{
	struct Work
	{
		uint64_t draw;
		bool submit;
	};
	Stream &stream;
	uint64_t draws = 0, submits = 0, checksum = 0;
	void Record(const Work &work, const Arena &)
	{
		if (work.submit)
		{
			assert(work.draw == draws);
			++submits;
			stream.Submitted(stream.Progress().recorded);
			if (stream.ActiveHeader().reserved & 1)
				stream.Result().words[0] = draws;
		}
		else
		{
			assert(work.draw == draws);
			checksum = checksum * 13 + ++draws;
		}
	}
};
static uint64_t Run(bool synchronous)
{
	setenv("CEMU_LATTE_PIPELINE", "2", 1);
	Context context;
	Stream stream(1024, 4096);
	SubmitRecorder recorder{stream};
	stream.Start(context, recorder, [] {}, [] {});
	constexpr uint32_t frames = 10, draws = 5000;
	uint64_t replies = 0;
	const auto start = std::chrono::steady_clock::now();
	auto submit = [&](uint64_t draw, bool waitIdle) {
		const bool reply = synchronous || waitIdle;
		const auto sequence = stream.Execute<SubmitRecorder::Work, &SubmitRecorder::Record>(context, Opcode::Submit, {draw, true}, recorder, reply);
		if (reply)
		{
			stream.Wait(sequence);
			assert(stream.TakeReply(sequence).words[0] == draw);
			++replies;
		}
	};
	for (uint32_t frame = 0; frame < frames; ++frame)
	{
		for (uint32_t draw = 0; draw < draws; ++draw)
		{
			const uint64_t total = uint64_t(frame) * draws + draw;
			stream.Execute<SubmitRecorder::Work, &SubmitRecorder::Record>(context, Opcode::Draw, {total, false}, recorder);
			if ((draw + 1) % 700 == 0)
				submit(total + 1, false);
		}
		submit(uint64_t(frame + 1) * draws, true);
		assert(stream.Progress().recorded == context.Sequence());
		context.EndFrame();
	}
	stream.Stop(context, recorder);
	assert(recorder.draws == frames * draws && recorder.submits == frames * 8);
	assert(replies == frames * (synchronous ? 8 : 1));
	const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / frames;
	std::cout << (synchronous ? "Synchronous" : "Asynchronous") << " nonblocking submits: " << ms << " ms/5000 draws; " << replies / frames << " replies/frame; reply-wait " << stream.Metrics().replyWaitNs / (1e6 * frames) << " ms/frame\n";
	return recorder.checksum;
}
int main()
{
	const auto before = Run(true);
	const auto after = Run(false);
	if (before != after)
		std::abort();
}
