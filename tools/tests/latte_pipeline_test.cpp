#include "Cafe/HW/Latte/Renderer/D3D12/LattePipeline.h"

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

#include <iostream>
#include <numeric>

using namespace LattePipeline;

template <class F>
static void Throws(F &&function)
{
	bool threw = false;
	try
	{
		function();
	}
	catch (const std::logic_error &)
	{
		threw = true;
	}
	assert(threw);
}

static void RingTests()
{
	for (size_t capacity : {1, 2, 3, 16})
	{
		Ring<uint64_t> ring(capacity);
		uint64_t value = 0;
		assert(!ring.Pop(value));
		for (uint64_t lap = 0; lap < 1000; ++lap)
		{
			for (uint64_t i = 0; i < capacity; ++i)
				assert(ring.Push(lap * capacity + i));
			assert(!ring.Push(UINT64_MAX)); // deliberate backpressure, no overwrite
			for (uint64_t i = 0; i < capacity; ++i)
			{
				assert(ring.Pop(value));
				assert(value == lap * capacity + i);
			}
			assert(ring.Empty());
		}
	}
	Ring<uint64_t> leaseRing(1);
	auto *write = leaseRing.TryWrite();
	assert(write);
	*write = 42;
	assert(!leaseRing.Peek());
	leaseRing.PublishWrite();
	assert(*leaseRing.Peek() == 42 && !leaseRing.TryWrite());
	leaseRing.ReleaseRead();
	assert(leaseRing.TryWrite());
	struct Value
	{
		uint64_t sequence;
		std::array<uint64_t, 16> bytes;
	};
	Ring<Value> ring(3);
	constexpr uint64_t count = 100000;
	std::thread producer([&] {
		for (uint64_t i = 0; i < count; ++i)
		{
			Value value{i, {}};
			value.bytes.fill(i ^ 0xfeedabcddcba1234ull);
			while (!ring.Push(value))
				std::this_thread::yield();
		}
	});
	for (uint64_t i = 0; i < count; ++i)
	{
		Value value;
		while (!ring.Pop(value))
			std::this_thread::yield();
		assert(value.sequence == i);
		for (auto byte : value.bytes)
			assert(byte == (i ^ 0xfeedabcddcba1234ull));
	}
	producer.join();
	assert(ring.Empty());
}

struct RecordingProbe
{
	std::atomic<bool> &entered, &released;
	void Record(const uint32_t &, const Arena &)
	{
		entered.store(true, std::memory_order_release);
		while (!released.load(std::memory_order_acquire))
			std::this_thread::yield();
	}
};

static void OwnershipTests()
{
	Context host;
	std::thread latte([&] { host.BindFrontThread(); host.CheckFront(); host.CheckBack(); host.BeginOperation(); });
	latte.join();
	host.BindFrontThread();
	host.CheckFront();
	host.CheckBack();
	Context context;
	std::atomic<bool> entered{false}, checked{false};
	std::thread back([&] {
		context.BindBackThread();
		context.EnterRecording();
		entered.store(true, std::memory_order_release);
		while (!checked.load(std::memory_order_acquire))
			std::this_thread::yield();
		context.LeaveRecording();
	});
	while (!entered.load(std::memory_order_acquire))
		std::this_thread::yield();
	// A busy back actor must not make the real front look like a recursive
	// callback or cause native wrappers to bypass their operation stream.
	context.CheckFront();
	checked.store(true, std::memory_order_release);
	back.join();
	context.BindBackThread();
	entered.store(false);
	checked.store(false);
	std::atomic<Stream *> activeStream{nullptr};
	std::thread isolatedConsumer([&] {
		Context local;
		Stream stream(1, 64);
		RecordingProbe probe{entered, checked};
		activeStream.store(&stream, std::memory_order_release);
		stream.Execute<uint32_t, &RecordingProbe::Record>(local, Opcode::Control, 42, probe);
		stream.Stop(local, probe);
	});
	while (!entered.load(std::memory_order_acquire))
		std::this_thread::yield();
	assert(!activeStream.load(std::memory_order_acquire)->Recording());
	checked.store(true, std::memory_order_release);
	isolatedConsumer.join();
}

static void RegistryPublicationTest()
{
	Registry registry(64);
	Ring<ResourceId> ring(2);
	std::array<uint32_t, 10000> objects{};
	std::thread producer([&] {
		for (uint32_t i = 0; i < objects.size(); ++i)
		{
			objects[i] = i;
			const auto id = registry.Insert(&objects[i]);
			while (!ring.Push(id))
				std::this_thread::yield();
		}
	});
	for (uint32_t i = 0; i < objects.size(); ++i)
	{
		ResourceId id;
		while (!ring.Pop(id))
			std::this_thread::yield();
		assert(*registry.Get<uint32_t>(id) == i);
		registry.Use(id, i + 1, i + 1);
		registry.Release(id);
		registry.Retire(i + 1, i + 1);
	}
	producer.join();
}

static void ArenaPublicationTest()
{
	Arena arena(256);
	Ring<Span> ring(2);
	constexpr uint64_t count = 10000;
	std::thread producer([&] {
		for (uint64_t i = 0; i < count; ++i)
		{
			std::array<uint64_t, 8> source;
			source.fill(i);
			Span span;
			while (!arena.Copy(std::as_bytes(std::span{source}), span))
				std::this_thread::yield();
			while (!ring.Push(span))
				std::this_thread::yield();
		}
	});
	for (uint64_t i = 0; i < count; ++i)
	{
		Span span;
		while (!ring.Pop(span))
			std::this_thread::yield();
		std::array<uint64_t, 8> result;
		const auto bytes = arena.Read(span);
		std::memcpy(result.data(), bytes.data(), bytes.size());
		for (auto value : result)
			assert(value == i);
		arena.Retire(span.position + span.size);
	}
	producer.join();
}

static void StateBlockTests()
{
	StateBlocks<uint64_t, 2> blocks;
	const auto a = blocks.Intern(1, 1, 0);
	assert(a && blocks.Get(a) == 1);
	assert(blocks.Intern(1, 2, 0) == a);
	const auto b = blocks.Intern(2, 3, 0);
	assert(b && b != a);
	assert(!blocks.Intern(3, 4, 1));
	const auto c = blocks.Intern(3, 4, 2);
	assert(c != a);
	assert(blocks.Get(c) == 3 && blocks.Get(b) == 2);
	Throws([&] { blocks.Get(a); });
}

static void LifetimeTests()
{
	Registry registry;
	int object = 42, replacement = 43;
	const auto old = registry.Insert(&object);
	registry.Use(old, 12, 20);
	registry.Release(old);
	Throws([&] { registry.Use(old, 13, 21); });
	registry.Retire(11, 21);
	assert(!registry.Resolve(old));
	assert(registry.Insert(&replacement).slot != old.slot);
	registry.Retire(12, 19);
	assert(!registry.Resolve(old));
	assert(registry.Insert(&replacement).slot != old.slot);
	registry.Retire(12, 20);
	assert(!registry.Resolve(old));
	const auto fresh = registry.Insert(&replacement);
	assert(fresh.slot == old.slot && fresh.generation != old.generation);
	assert(*registry.Get<int>(fresh) == 43);
	Throws([&] { registry.Get<int>(old); });
	Tickets tickets;
	tickets.Record(5);
	tickets.Submit(5);
	assert(tickets.completed == 0); // recording/submitting do not imply completion
	Throws([&] { tickets.Complete(6); });
	tickets.Complete(5);
	tickets.Record(7);
	Throws([&] { tickets.Record(6); });
	Throws([&] { tickets.Submit(8); });
	assert(tickets.completed == 5 && tickets.recorded == 7);
}

static void ArenaTests()
{
	Arena arena(64);
	std::array<std::byte, 32> bytes;
	bytes.fill(std::byte{0x42});
	Span first, second, third;
	assert(arena.Copy(bytes, first));
	assert(arena.Copy(bytes, second));
	assert(!arena.Copy(bytes, third)); // allocation pressure
	assert(arena.Read(first).front() == std::byte{0x42});
	arena.Retire(first.position + first.size);
	assert(arena.Copy(bytes, third)); // wraps without reusing a live span
	Throws([&] { arena.Read(first); });
	assert(arena.Read(second).back() == std::byte{0x42});
	assert(arena.Read(third).back() == std::byte{0x42});
	arena.Retire(arena.End());
	arena.ResetEmpty();
	Arena oddArena(65);
	for (size_t i = 0; i < 1000; ++i)
	{
		const size_t size = i % 3 == 0 ? 17 : 32;
		Span span;
		assert(oddArena.Copy(std::span{bytes}.first(size), span));
		assert(reinterpret_cast<uintptr_t>(oddArena.Read(span).data()) % 16 == 0);
		oddArena.Retire(oddArena.End());
	}
	std::array<std::byte, 65> tooLarge{};
	assert(!arena.Copy(tooLarge, first));
}

struct MockOperation
{
	ResourceId resource;
	Span data;
	uint32_t command, value;
};
struct MockRecorder
{
	Registry resources;
	std::vector<uint64_t> commands;
	uint32_t descriptors = 0;
	void Record(const MockOperation &operation, const Arena &arena)
	{
		const auto *resource = resources.Get<uint32_t>(operation.resource);
		// Model heap rollover, which invalidates bound root tables without
		// consuming or changing the immutable CPU payload.
		if (++descriptors == 3)
		{
			commands.push_back(0xff);
			descriptors = 0;
		}
		commands.push_back(operation.command);
		commands.push_back(*resource);
		commands.push_back(operation.value);
		for (auto value : arena.Read(operation.data))
			commands.push_back(std::to_integer<uint8_t>(value));
	}
};
static std::vector<uint64_t> RoundTrip(bool queued, size_t capacity)
{
	setenv("CEMU_LATTE_PIPELINE", queued ? "1" : "0", 1);
	Context context;
	Stream stream(capacity, 128);
	MockRecorder recorder;
	uint32_t value = 17;
	auto id = recorder.resources.Insert(&value);
	for (uint32_t i = 0; i < 1000; ++i)
	{
		std::array<std::byte, 64> source;
		source.fill(static_cast<std::byte>(i));
		Span span;
		assert(stream.Staging().Copy(source, span));
		MockOperation packet{id, span, i % 7, i};
		source.fill(std::byte{0}); // no guest-memory pointer survives preparation
		stream.Execute<MockOperation, &MockRecorder::Record>(context, Opcode::Draw, packet, recorder);
		assert(stream.Progress().recorded == context.Sequence());
		if (i % 5 == 0)
			context.EndFrame();
		if (i % 13 == 0)
		{
			recorder.resources.Release(id);
			recorder.resources.Retire(context.Sequence(), 0);
			id = recorder.resources.Insert(&value);
		}
	}
	stream.Stop(context, recorder);
	Throws([&] { stream.Execute<MockOperation, &MockRecorder::Record>(context, Opcode::Shutdown, MockOperation{}, recorder); });
	return recorder.commands;
}

struct MockDrawRecorder
{
	StateBlocks<DrawBlock, 2, DrawBlockEqual> blocks;
	std::vector<uint64_t> commands;
	void Emit(const DrawPacket &packet, const DrawBlock &block, std::span<const std::byte> uniform, std::span<const std::byte> runtime)
	{
		// A deterministic mock command stream covering native draw arguments,
		// pipeline/FBO/index identities, complete stage mappings and byte payloads.
		for (auto id : {packet.pipeline, packet.framebuffer, packet.indices})
		{
			commands.push_back(id.slot);
			commands.push_back(id.generation);
		}
		for (auto value : {packet.baseVertex, packet.baseInstance, packet.instanceCount, packet.count, packet.indexCount, packet.indexType, packet.stencilRef})
			commands.push_back(value);
		for (uint32_t i = 0; i < block.stageCount; ++i)
		{
			const auto &stage = block.stages[i];
			commands.push_back(static_cast<uint32_t>(stage.stage));
			for (const auto &range : stage.uniforms)
			{
				commands.push_back(range.offset);
				commands.push_back(range.size);
			}
			for (uint32_t j = 0; j < stage.textureCount; ++j)
			{
				const auto &view = stage.textures[j];
				commands.push_back(view.view.slot);
				commands.push_back(view.view.generation);
				commands.push_back(view.swizzle);
				commands.push_back(static_cast<uint32_t>(view.reg));
			}
			const auto samplerBytes = std::as_bytes(std::span{stage.samplers, stage.samplerCount});
			for (auto byte : samplerBytes)
				commands.push_back(std::to_integer<uint8_t>(byte));
		}
		for (auto bytes : {uniform, runtime})
			for (auto byte : bytes)
				commands.push_back(std::to_integer<uint8_t>(byte));
	}
	void Record(const DrawPacket &packet, const Arena &arena)
	{
		Emit(packet, blocks.Get(packet.state), arena.Read(packet.uniformBytes[0]), arena.Read(packet.runtime));
	}
};
static std::vector<uint64_t> DrawRoundTrip(bool queued, size_t capacity)
{
	setenv("CEMU_LATTE_PIPELINE", queued ? "1" : "0", 1);
	Context context;
	Stream stream(capacity, 256, 1);
	MockDrawRecorder recorder;
	for (uint32_t i = 0; i < 100; ++i)
	{
		DrawBlock block{};
		block.stageCount = 3;
		for (uint32_t stageIndex = 0; stageIndex < 3; ++stageIndex)
		{
			auto &stage = block.stages[stageIndex];
			stage.stage = static_cast<D3D12Const::Stage>(stageIndex);
			stage.samplerCount = 2;
			stage.textureCount = 2;
			stage.uniforms[0] = {i * 256, 256};
			stage.textures[0] = {{i % 3, i / 3 + 1}, i, 0, Latte::E_DIM::DIM_2D};
			stage.textures[1] = {{}, i, -2, Latte::E_DIM::DIM_2D};
			stage.samplers[0].words[0] = i;
		}
		std::array<std::byte, 64> uniform, runtime;
		uniform.fill(static_cast<std::byte>(i));
		runtime.fill(static_cast<std::byte>(i + 1));
		DrawPacket packet{};
		packet.pipeline = {i % 2, 1};
		packet.framebuffer = {2, 1};
		packet.indices = {3, i + 1};
		packet.count = i + 1;
		packet.indexCount = i * 3;
		packet.indexType = i % 3;
		packet.stencilRef = i;
		packet.baseVertex = i * 5;
		packet.baseInstance = i * 2;
		packet.instanceCount = i + 2;
		if (queued)
		{
			packet.state = recorder.blocks.Intern(block, context.Sequence() + 1, stream.Progress().recorded);
			assert(packet.state);
			assert(stream.Staging().Copy(uniform, packet.uniformBytes[0]));
			assert(stream.Staging().Copy(runtime, packet.runtime));
			// All caller-owned state can change before recording.
			block.stages[0].textures[0].swizzle = UINT32_MAX;
			uniform.fill(std::byte{0xff});
			runtime.fill(std::byte{0xff});
			stream.Execute<DrawPacket, &MockDrawRecorder::Record>(context, Opcode::Draw, packet, recorder);
		}
		else
			recorder.Emit(packet, block, uniform, runtime);
	}
	stream.Stop(context, recorder);
	return recorder.commands;
}

struct ThreadedRecorder
{
	Context &context;
	Stream &stream;
	ThreadedRecorder(Context &c, Stream &s) : context(c), stream(s)
	{
	}
	Registry registry{16};
	StateBlocks<uint64_t, 4> blocks;
	uint64_t next = 0;
	std::atomic<bool> hold{false}, entered{false};
	struct Work
	{
		ResourceId id;
		BlockId block;
		Span bytes;
		uint64_t value;
		bool fail = false;
	};
	void Record(const Work &work, const Arena &arena)
	{
		context.CheckBack();
		entered.store(true);
		while (hold.load())
			std::this_thread::yield();
		if (work.fail)
			throw std::runtime_error("Injected device loss");
		assert(work.value == next++);
		assert(*registry.Get<uint64_t>(work.id) == work.value);
		assert(blocks.Get(work.block) == work.value);
		assert(arena.Read(work.bytes)[0] == std::byte(work.value & 255));
		registry.Use(work.id, stream.ActiveHeader().sequence, next);
		registry.Release(work.id);
		registry.Retire(stream.ActiveHeader().sequence, next, &stream.RetiredResources());
		stream.Submitted(stream.Progress().recorded);
		stream.Completed(stream.Progress().submitted);
		stream.Result().words[0] = work.value;
	}
};

static void WorkerTests()
{
	setenv("CEMU_LATTE_PIPELINE", "2", 1);
	for (size_t capacity : {1, 2, 3, 8})
	{
		Context context;
		Stream stream(capacity, 256, 1);
		ThreadedRecorder recorder{context, stream};
		uint64_t replies = 0;
		stream.SetCompletionHandler([&](const Completion &result) {
			context.CheckFront();
			recorder.registry.Recycle(result.retiredResources);
			assert(result.words[0] == replies++);
			assert(result.tickets.recorded == replies);
			assert(result.tickets.completed <= result.tickets.submitted);
			assert(result.tickets.submitted < result.tickets.recorded);
		});
		stream.Start(context, recorder, [] {}, [] {});
		std::array<uint64_t, 4> values{}, leases{};
		for (uint64_t value = 0; value < 20000; ++value)
		{
			stream.Poll();
			auto block = recorder.blocks.Intern(value, context.Sequence() + 1, stream.Progress().recorded);
			while (!block)
			{
				stream.Wait(stream.Progress().recorded + 1);
				block = recorder.blocks.Intern(value, context.Sequence() + 1, stream.Progress().recorded);
			}
			// Four immutable CPU slots, reused only after reverse acknowledgment.
			if (leases[value % 4] > stream.Progress().recorded)
				stream.Wait(leases[value % 4]);
			values[value % 4] = value;
			Span span;
			const std::array<std::byte, 32> bytes{std::byte(value & 255)};
			if (!stream.Staging().Copy(bytes, span))
			{
				stream.Finish();
				stream.Staging().ResetEmpty();
				assert(stream.Staging().Copy(bytes, span));
			}
			auto id = recorder.registry.Insert(&values[value % 4]);
			ThreadedRecorder::Work work{id, block, span, value};
			leases[value % 4] = stream.Execute<ThreadedRecorder::Work, &ThreadedRecorder::Record>(context, Opcode::Draw, work, recorder, value % 113 == 0);
			if (value % 113 == 0)
			{
				stream.Finish();
				const auto reply = stream.TakeReply(leases[value % 4]);
				assert(reply.words[0] == value);
				stream.Exclusive(context, [&] { context.CheckBack(); assert(recorder.next == value + 1); });
			}
		}
		// Stop is out-of-band; it must drain even if no control slot is free.
		stream.Stop(context, recorder);
		assert(replies == 20000 && recorder.next == replies);
		assert(stream.Metrics().highWater <= capacity);
		context.CheckBack();
	}
	for (bool cancel : {false, true})
	{
		struct HeldRecorder
		{
			std::atomic<bool> entered{false}, release{false};
			uint64_t count = 0;
			void Record(const uint64_t &, const Arena &)
			{
				entered.store(true);
				while (!release.load())
					std::this_thread::yield();
				++count;
			}
		} held;
		Context context;
		Stream stream(1, 64, 1);
		stream.Start(context, held, [] {}, [] {});
		stream.Execute<uint64_t, &HeldRecorder::Record>(context, Opcode::Draw, 0, held);
		while (!held.entered.load())
			std::this_thread::yield();
		// The consumer still holds the only slot until Record returns.
		assert(stream.Occupancy() == 1);
		std::thread release([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			held.release.store(true);
		});
		if (cancel)
			stream.Cancel();
		bool failed = false;
		try
		{
			stream.Stop(context, held);
		}
		catch (const std::runtime_error &)
		{
			failed = true;
		}
		release.join();
		assert(failed == cancel && held.count == 1);
	}
	// Cross-thread UI handoffs also drain a submitted prefix and restore owner.
	{
		struct Counter
		{
			uint64_t value = 0;
			void Record(const uint32_t &, const Arena &)
			{
				++value;
			}
		} counter;
		Context context;
		Stream stream(2, 64);
		std::atomic<uint32_t> idleCalls{0};
		stream.Start(context, counter, [&] { ++idleCalls; }, [] {});
		stream.Execute<uint32_t, &Counter::Record>(context, Opcode::Draw, 1, counter);
		std::thread ui([&] { stream.Exclusive(context, [&] { context.CheckBack(); assert(counter.value == 1); }); });
		ui.join();
		stream.Execute<uint32_t, &Counter::Record>(context, Opcode::Draw, 1, counter);
		stream.Finish();
		stream.Exclusive(context, [&] {
			context.RunFrontCallback([&] {
				context.CheckFront();
				context.CheckBack();
				Throws([&] { stream.Execute<uint32_t, &Counter::Record>(context, Opcode::Draw, 1, counter); });
			});
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		stream.Stop(context, counter);
		assert(counter.value == 2 && idleCalls.load() < 1000);
	}
	{
		struct Recorder
		{
			void Record(const uint32_t &, const Arena &)
			{
			}
		} recorder;
		Context context;
		Stream stream(1, 64);
		stream.Start(context, recorder, [] {}, [] {});
		bool failed = false;
		try
		{
			stream.Exclusive(context, [] { throw std::runtime_error("Injected surface/device failure"); });
		}
		catch (const std::runtime_error &)
		{
			failed = true;
		}
		assert(failed);
		try
		{
			stream.Stop(context, recorder);
		}
		catch (const std::runtime_error &)
		{
		}
	}
	{
		Arena arena(64);
		std::array<std::byte, 32> bytes{};
		Span first, second;
		assert(arena.Copy(bytes, first));
		arena.Retire(arena.End());
		assert(!arena.Prepare(48));
		arena.ResetEmpty();
		assert(arena.Prepare(48));
		assert(arena.Copy(std::span(bytes).first(16), first));
		assert(arena.Copy(bytes, second));
		assert(second.position == first.position + 16);
		assert(arena.Read(first).size() == 16 && arena.Read(second).size() == 32);
		assert(!arena.Prepare(64));
	}
	// A back exception wakes capacity/reply waiters and fails every later ticket.
	{
		Context context;
		Stream stream(1, 64);
		ThreadedRecorder recorder{context, stream};
		stream.Start(context, recorder, [] {}, [] {});
		ThreadedRecorder::Work work{};
		work.fail = true;
		const auto ticket = stream.Execute<ThreadedRecorder::Work, &ThreadedRecorder::Record>(context, Opcode::Draw, work, recorder);
		bool failed = false;
		try
		{
			stream.Wait(ticket);
		}
		catch (const std::runtime_error &)
		{
			failed = true;
		}
		assert(failed);
		try
		{
			stream.Stop(context, recorder);
		}
		catch (const std::runtime_error &)
		{
		}
	}
	unsetenv("CEMU_LATTE_PIPELINE");
	std::cout << "Latte worker: tiny rings, ordered replies, concurrent generations, handoff, full-ring shutdown, reverse saturation, cancellation, idle sleep and device failure passed\n";
}

int main()
{
	WorkerTests();
	OwnershipTests();
	RingTests();
	RegistryPublicationTest();
	ArenaPublicationTest();
	StateBlockTests();
	LifetimeTests();
	ArenaTests();
	const auto directDraws = DrawRoundTrip(false, 1);
	for (size_t capacity : {1, 2, 3})
		assert(DrawRoundTrip(true, capacity) == directDraws);
	const auto direct = RoundTrip(false, 1);
	for (size_t capacity : {1, 2, 3, 1024})
		assert(RoundTrip(true, capacity) == direct);
	std::cout << "Latte pipeline: wrap, release/acquire, backpressure, generations, tickets, arena pressure, rollover, round-trip and shutdown passed\n";
}
