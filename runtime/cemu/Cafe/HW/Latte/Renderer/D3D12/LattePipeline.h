#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

// Platform-neutral handoff contracts, also exercised by the host tests.
namespace LattePipeline
{
inline bool EnvironmentFlag(const char *name)
{
	const char *value = std::getenv(name);
	return value && std::strcmp(value, "1") == 0;
}

class Context
{
  public:
	Context() : m_front(std::this_thread::get_id()), m_back(m_front)
	{
	}
	bool Queued() const
	{
		return m_mode != 0;
	}
	bool Threaded() const
	{
		return m_mode == 2;
	}
	void CheckFront() const
	{
		Check(m_front, "front");
#if defined(NDEBUG) && !defined(_DEBUG)
		if (!m_checks)
			return;
#endif
		if (s_recording == this && s_frontCallback != this)
		{
			std::fputs("Latte pipeline: front access from recorder\n", stderr);
			std::abort();
		}
	}
	template <class F>
	void RunFrontCallback(F function)
	{
		Check(m_front, "front callback");
		CheckBack();
		const auto previous = s_frontCallback;
		s_frontCallback = this;
		try
		{
			function();
		}
		catch (...)
		{
			s_frontCallback = previous;
			throw;
		}
		s_frontCallback = previous;
	}
	void CheckBack() const
	{
		Check(m_back, "back");
	}
	// Construction belongs to the CoreWindow caller; Initialize transfers both
	// actors to Latte before starting its worker. No packets may be live here.
	void BindFrontThread()
	{
		if (m_backActive.load(std::memory_order_acquire))
			std::abort();
		m_front = m_back = std::this_thread::get_id();
	}
	// Ownership handoff is only legal at a drained lifecycle boundary.
	void BindBackThread()
	{
		if (m_backActive.load(std::memory_order_acquire))
			std::abort();
		m_back = std::this_thread::get_id();
	}
	void EnterRecording()
	{
		CheckBack();
		if (ChecksEnabled())
		{
			s_recording = this;
			m_backActive.store(true, std::memory_order_release);
		}
	}
	void LeaveRecording()
	{
		if (ChecksEnabled())
		{
			s_recording = nullptr;
			m_backActive.store(false, std::memory_order_release);
		}
	}
	uint64_t BeginOperation()
	{
		CheckFront();
		return ++m_sequence;
	}
	uint64_t Sequence() const
	{
		return m_sequence;
	}
	uint64_t Frame() const
	{
		return m_frame;
	}
	void EndFrame()
	{
		CheckFront();
		++m_frame;
	}

  private:
	bool ChecksEnabled() const
	{
#if defined(NDEBUG) && !defined(_DEBUG)
		return m_checks;
#else
		return true;
#endif
	}
	void Check(std::thread::id owner, const char *role) const
	{
#if defined(NDEBUG) && !defined(_DEBUG)
		if (!m_checks)
			return;
#endif
		if (owner != std::this_thread::get_id())
		{
			std::fprintf(stderr, "Latte pipeline: %s ownership violation\n", role);
			std::abort();
		}
	}
	std::thread::id m_front;
	std::thread::id m_back;
	const bool m_checks = EnvironmentFlag("CEMU_LATTE_PIPELINE_CHECKS");
	const unsigned m_mode = [] { const char *v = std::getenv("CEMU_LATTE_PIPELINE");
		return v && std::strcmp(v, "2") == 0 ? 2u : EnvironmentFlag("CEMU_LATTE_PIPELINE") ? 1u : 0u; }();
	inline static thread_local const Context *s_recording = nullptr;
	inline static thread_local const Context *s_frontCallback = nullptr;
	std::atomic<bool> m_backActive{false};
	uint64_t m_sequence = 0;
	uint64_t m_frame = 1;
};
} // namespace LattePipeline

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace LattePipeline
{
struct ResourceId
{
	uint32_t slot = 0;
	uint32_t generation = 0;
	explicit operator bool() const
	{
		return generation != 0;
	}
	bool operator==(const ResourceId &) const = default;
};
static_assert(sizeof(ResourceId) == 8);
inline void *OpaqueId(ResourceId id)
{
	return reinterpret_cast<void *>((static_cast<uintptr_t>(id.generation) << 32) | id.slot);
}
inline ResourceId FromOpaqueId(void *id)
{
	const auto bits = reinterpret_cast<uintptr_t>(id);
	return {static_cast<uint32_t>(bits), static_cast<uint32_t>(bits >> 32)};
}

// CPU consumption and GPU completion are independent lifetime requirements.
class Registry
{
	struct Entry
	{
		void *object = nullptr;
		uint64_t lastSequence = 0, lastFence = 0;
		uint32_t generation = 1;
		bool retiring = false;
	};

  public:
	explicit Registry(uint32_t capacity = 262144) : m_capacity(capacity), m_entries(std::make_unique<Entry[]>(capacity))
	{
		m_free.reserve(capacity);
	}
	ResourceId Insert(void *object)
	{
		if (!object)
			return {};
		// Creation/retirement may coordinate allocation. Lookup and pinning on
		// the draw path never take this lock, and entry addresses never move.
		std::lock_guard lock(m_allocationMutex);
		uint32_t slot;
		const auto count = m_count.load(std::memory_order_relaxed);
		if (m_free.empty())
		{
			if (count == m_capacity)
				throw std::runtime_error("Latte resource registry exhausted");
			slot = count;
		}
		else
		{
			slot = m_free.back();
			m_free.pop_back();
		}
		auto &e = m_entries[slot];
		e.object = object;
		e.lastSequence = e.lastFence = 0;
		if (slot == count)
			m_count.store(count + 1, std::memory_order_release);
		return {slot, e.generation};
	}
	void *Resolve(ResourceId id) const
	{
		if (!id || id.slot >= m_capacity)
			return nullptr;
		const auto &e = m_entries[id.slot];
		return e.generation == id.generation && !e.retiring ? e.object : nullptr;
	}
	template <class T>
	T *Get(ResourceId id) const
	{
		auto *p = static_cast<T *>(Resolve(id));
		if (!p)
			throw std::logic_error("Stale Latte resource ID");
		return p;
	}
	void Use(ResourceId id, uint64_t sequence, uint64_t fence)
	{
		if (!id)
			return;
		Get<void>(id);
		auto &e = m_entries[id.slot];
		e.lastSequence = std::max(e.lastSequence, sequence);
		e.lastFence = std::max(e.lastFence, fence);
	}
	void Release(ResourceId id)
	{
		if (!id)
			return;
		Get<void>(id);
		m_retiring.push_back(id.slot);
		m_entries[id.slot].retiring = true;
	}
	void Retire(uint64_t consumed, uint64_t completed, std::vector<ResourceId> *ready = nullptr)
	{
		// Only the recorder owns this list. Do not scan every live scene resource
		// or contend with front-side Insert when nothing is awaiting retirement.
		if (m_retiring.empty())
			return;
		std::lock_guard lock(m_allocationMutex);
		size_t pending = 0;
		for (const auto i : m_retiring)
		{
			auto &e = m_entries[i];
			if (e.lastSequence > consumed || e.lastFence > completed)
			{
				m_retiring[pending++] = i;
				continue;
			}
			e.object = nullptr;
			e.retiring = false;
			// Never wrap a generation into an old valid ID.
			if (e.generation != std::numeric_limits<uint32_t>::max())
			{
				++e.generation;
				if (ready)
					ready->push_back({i, e.generation});
				else
					m_free.push_back(i);
			}
		}
		m_retiring.resize(pending);
	}

	// Threaded retirement transfers free generations through reverse messages.
	// Front returns them to the shared allocation pool after consuming that reply.
	void Recycle(std::span<const ResourceId> ready)
	{
		if (ready.empty())
			return;
		std::lock_guard lock(m_allocationMutex);
		for (auto id : ready)
		{
			if (!id || id.slot >= m_capacity)
				throw std::logic_error("Invalid Latte recycled resource ID");
			const auto &entry = m_entries[id.slot];
			if (entry.object || entry.retiring || entry.generation != id.generation)
				throw std::logic_error("Live Latte resource recycled");
			m_free.push_back(id.slot);
		}
	}

  private:
	const uint32_t m_capacity;
	std::unique_ptr<Entry[]> m_entries;
	std::atomic<uint32_t> m_count{0};
	std::vector<uint32_t> m_free;
	std::vector<uint32_t> m_retiring;
	std::mutex m_allocationMutex;
};

// Block slots turn over at draw frequency. Pack their small slot index into a
// 64-bit generation token instead of exhausting a 32-bit generation in hours.
struct BlockId
{
	uint64_t value = 0;
	explicit operator bool() const
	{
		return value != 0;
	}
	bool operator==(const BlockId &) const = default;
};
static_assert(sizeof(BlockId) == 8);

// Fixed immutable front-owned blocks. Slot replacement is legal only after
// consumption; equality is always checked (a hash alone never establishes a hit).
template <class T>
struct ByteEqual
{
	bool operator()(const T &a, const T &b) const
	{
		return std::memcmp(&a, &b, sizeof(T)) == 0;
	}
};
template <class T, size_t Capacity = 4, class Equal = ByteEqual<T>>
class StateBlocks
{
	struct Entry
	{
		T value{};
		uint64_t sequence = 0;
		uint64_t generation = 0;
	};

  public:
	BlockId Intern(const T &value, uint64_t sequence, uint64_t consumed)
	{
		static_assert(std::is_trivially_copyable_v<T>);
		auto &last = m_entries[m_last];
		if (last.generation && Equal{}(last.value, value))
		{
			last.sequence = sequence;
			return {last.generation * Capacity + m_last};
		}
		for (size_t offset = 0; offset < Capacity; ++offset)
		{
			const auto i = (m_last + offset) % Capacity;
			auto &entry = m_entries[i];
			if (entry.sequence <= consumed && entry.generation != UINT64_MAX / Capacity)
			{
				entry.value = value;
				entry.sequence = sequence;
				++entry.generation;
				m_last = i;
				return {entry.generation * Capacity + i};
			}
		}
		return {};
	}
	const T &Get(BlockId id) const
	{
		const uint64_t generation = id.value / Capacity;
		const size_t slot = id.value % Capacity;
		if (!generation || m_entries[slot].generation != generation)
			throw std::logic_error("Stale Latte immutable state block");
		return m_entries[slot].value;
	}

  private:
	std::array<Entry, Capacity> m_entries{};
	size_t m_last = 0;
};

// A bounded SPSC ring; only the producer writes m_producer, only the consumer
// writes m_consumer. Slots/arena bytes become visible with the same release edge.
// Capacity one is supported (monotonic cursors distinguish full from empty).
template <class T>
class Ring
{
  public:
	explicit Ring(size_t capacity) : m_capacity(capacity), m_slots(std::make_unique<T[]>(capacity))
	{
		if (!capacity)
			throw std::invalid_argument("Zero Latte ring capacity");
	}
	T *TryWrite()
	{
		const uint64_t write = m_producer.load(std::memory_order_relaxed);
		if (write - m_consumer.load(std::memory_order_acquire) == m_capacity)
			return nullptr;
		return &m_slots[Index(write)];
	}
	void PublishWrite()
	{
		m_producer.store(m_producer.load(std::memory_order_relaxed) + 1, std::memory_order_release);
	}
	const T *Peek() const
	{
		const uint64_t read = m_consumer.load(std::memory_order_relaxed);
		if (read == m_producer.load(std::memory_order_acquire))
			return nullptr;
		return &m_slots[Index(read)];
	}
	void ReleaseRead()
	{
		m_consumer.store(m_consumer.load(std::memory_order_relaxed) + 1, std::memory_order_release);
	}
	bool Push(const T &value)
	{
		auto *slot = TryWrite();
		if (!slot)
			return false;
		*slot = value;
		PublishWrite();
		return true;
	}
	bool Pop(T &value)
	{
		const auto *slot = Peek();
		if (!slot)
			return false;
		value = std::move(*const_cast<T *>(slot));
		ReleaseRead();
		return true;
	}
	uint64_t Published() const
	{
		return m_producer.load(std::memory_order_acquire);
	}
	uint64_t Consumed() const
	{
		return m_consumer.load(std::memory_order_acquire);
	}
	uint64_t Size() const
	{
		const auto read = Consumed();
		return Published() - read;
	}
	bool Empty() const
	{
		return m_consumer.load(std::memory_order_acquire) == m_producer.load(std::memory_order_acquire);
	}

  private:
	static_assert(std::atomic<uint64_t>::is_always_lock_free);
	size_t Index(uint64_t cursor) const
	{
		return (m_capacity & (m_capacity - 1)) == 0 ? cursor & (m_capacity - 1) : cursor % m_capacity;
	}
	const uint64_t m_capacity;
	std::unique_ptr<T[]> m_slots;
	alignas(64) std::atomic<uint64_t> m_producer{0};
	alignas(64) std::atomic<uint64_t> m_consumer{0};
};

struct Span
{
	uint64_t position = 0;
	uint32_t size = 0;
	uint32_t reserved = 0;
};
class Arena
{
  public:
	explicit Arena(size_t capacity) : m_capacity(capacity), m_bytes(std::make_unique<std::byte[]>(capacity))
	{
		if (!capacity || capacity > UINT32_MAX)
			throw std::invalid_argument("Invalid Latte arena capacity");
	}
	bool Copy(std::span<const std::byte> bytes, Span &out)
	{
		if (bytes.size() > m_capacity)
			return false;
		const uint64_t read = m_consumed.load(std::memory_order_acquire);
		const uint64_t offset = Offset(m_written);
		const uint64_t alignedOffset = (offset + 15) & ~uint64_t{15};
		// Alignment belongs to the physical buffer, not the absolute cursor:
		// a configured capacity need not be a multiple of sixteen bytes.
		const bool wrap = alignedOffset + bytes.size() > m_capacity;
		const uint64_t start = m_written + (wrap ? m_capacity - offset : alignedOffset - offset);
		const uint64_t destination = wrap ? 0 : alignedOffset;
		if (start + bytes.size() - read > m_capacity)
			return false;
		if (!bytes.empty())
			std::memcpy(m_bytes.get() + destination, bytes.data(), bytes.size());
		out = {start, static_cast<uint32_t>(bytes.size()), 0};
		m_written = start + bytes.size();
		return true;
	}
	// Reserve a contiguous operation group before writing any of its spans.
	// Skipping a tail is part of the next published arenaEnd, never a reset of
	// live worker bytes. Draw uniforms cannot straddle a capacity-sized gap.
	bool Prepare(size_t bytes)
	{
		if (bytes > m_capacity)
			return false;
		const auto read = m_consumed.load(std::memory_order_acquire);
		const auto offset = Offset(m_written);
		const auto aligned = (offset + 15) & ~uint64_t{15};
		const auto start = m_written + (aligned + bytes > m_capacity ? m_capacity - offset : aligned - offset);
		if (start + bytes - read > m_capacity)
			return false;
		m_written = start;
		return true;
	}
	std::span<const std::byte> Read(Span span) const
	{
		const uint64_t offset = Offset(span.position);
		if (span.position < m_consumed.load(std::memory_order_relaxed) || offset + span.size > m_capacity)
			throw std::logic_error("Retired Latte arena span");
		return {m_bytes.get() + offset, span.size};
	}
	uint64_t End() const
	{
		return m_written;
	}
	void Retire(uint64_t end)
	{
		if (end < m_consumed.load(std::memory_order_relaxed))
			throw std::logic_error("Invalid Latte arena retirement");
		m_consumed.store(end, std::memory_order_release);
	}
	// Only at an empty, synchronously drained boundary; avoids wrap padding
	// consuming capacity needed by a single operation's state block.
	void ResetEmpty()
	{
		if (m_consumed.load(std::memory_order_acquire) != m_written)
			throw std::logic_error("Live Latte arena reset");
		const auto next = ((m_written + m_capacity - 1) / m_capacity) * m_capacity;
		m_written = next;
		m_consumed.store(next, std::memory_order_release);
	}
	size_t Capacity() const
	{
		return m_capacity;
	}

  private:
	size_t Offset(uint64_t position) const
	{
		return (m_capacity & (m_capacity - 1)) == 0 ? position & (m_capacity - 1) : position % m_capacity;
	}
	const size_t m_capacity;
	std::unique_ptr<std::byte[]> m_bytes;
	uint64_t m_written = 0;
	alignas(64) std::atomic<uint64_t> m_consumed{0};
};

enum class Opcode : uint16_t
{
	Draw,
	BufferUpload,
	TextureUpload,
	IndexUpload,
	Create,
	Release,
	Copy,
	Clear,
	Bind,
	Query,
	Readback,
	Submit,
	Present,
	Control,
	Shutdown
};
struct Header
{
	Opcode opcode{};
	uint16_t bytes = 0;
	uint32_t reserved = 0;
	uint64_t sequence = 0, frame = 0;
};
struct Packet
{
	Header header;
	uint64_t arenaEnd = 0;
	// The target is a static, capture-free typed recorder, never a closure or a
	// pointer into mutable guest state. Resource references in payloads are IDs.
	void (*record)(void *, const Packet &, const Arena &) = nullptr;
	alignas(16) std::array<std::byte, 208> payload{};
	template <class T>
	T As() const
	{
		static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= 208);
		if (header.bytes != sizeof(T))
			throw std::logic_error("Invalid Latte packet payload");
		T value;
		std::memcpy(&value, payload.data(), sizeof(T));
		return value;
	}
};
static_assert(sizeof(Header) == 24);
static_assert(sizeof(Packet) == 256);

// Tickets are operation sequence numbers, not fence numbers. Submitted/completed
// progress must be attached to the last operation covered by that fence.
struct Tickets
{
	uint64_t recorded = 0, submitted = 0, completed = 0;
	void Record(uint64_t sequence)
	{
		if (sequence <= recorded)
			throw std::logic_error("Out-of-order Latte recording");
		recorded = sequence;
	}
	void Submit(uint64_t sequence)
	{
		if (sequence < submitted || sequence > recorded)
			throw std::logic_error("Invalid Latte submit ticket");
		submitted = sequence;
	}
	void Complete(uint64_t sequence)
	{
		if (sequence < completed || sequence > submitted)
			throw std::logic_error("Invalid Latte completion ticket");
		completed = sequence;
	}
};

inline size_t CapacityFromEnvironment(const char *name, size_t fallback, size_t minimum, size_t maximum)
{
	const char *value = std::getenv(name);
	if (!value)
		return fallback;
	char *end = nullptr;
	const auto parsed = std::strtoull(value, &end, 10);
	return end != value && !*end && parsed >= minimum && parsed <= maximum ? static_cast<size_t>(parsed) : fallback;
}

// Notifications are not ordering edges. Arm under the wait mutex, recheck the
// atomic epoch/cursors, then sleep. The hot path takes no mutex unless a sleeper
// has armed the event. Timed waits also service output deadlines without work.
class Event
{
  public:
	uint64_t Epoch() const
	{
		return m_epoch.load(std::memory_order_seq_cst);
	}
	void Signal()
	{
		m_epoch.fetch_add(1, std::memory_order_seq_cst);
		if (m_waiting.load(std::memory_order_seq_cst))
		{
			std::lock_guard lock(m_mutex);
			m_cv.notify_all();
		}
	}
	void Wait(uint64_t epoch)
	{
		// Short producer/consumer gaps should not pay a kernel wakeup. This is
		// bounded and uses processor pause hints; genuinely idle workers sleep.
		for (unsigned spin = 0; spin < 256; ++spin)
		{
			if (Epoch() != epoch)
				return;
#if defined(__x86_64__) || defined(_M_X64)
			_mm_pause();
#elif defined(__aarch64__)
			asm volatile("yield");
#else
			std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
		}
		std::unique_lock lock(m_mutex);
		m_waiting.fetch_add(1, std::memory_order_seq_cst);
		m_cv.wait_for(lock, std::chrono::milliseconds(1), [&] { return Epoch() != epoch; });
		m_waiting.fetch_sub(1, std::memory_order_seq_cst);
	}

  private:
	std::atomic<uint64_t> m_epoch{0};
	std::atomic<uint32_t> m_waiting{0};
	std::mutex m_mutex;
	std::condition_variable m_cv;
};

struct Completion
{
	uint64_t sequence = 0;
	uint64_t backBusyNs = 0, backIdleNs = 0, completedFence = 0;
	Tickets tickets;
	uint32_t kind = 0;
	bool replyRequested = false;
	ResourceId resource;
	std::array<uint64_t, 8> words{};
	// Only exceptional CPU-visible readbacks/profile snapshots allocate bytes.
	std::vector<std::byte> bytes;
	std::vector<ResourceId> retiredResources;
};
struct PipelineTiming
{
	uint64_t backBusyNs = 0, backIdleNs = 0;
	uint64_t backpressureNs = 0, replyWaitNs = 0, highWater = 0, completionMessages = 0;
};

class Stream
{
  public:
	Stream(size_t capacity = 1024, size_t arenaBytes = 16 * 1024 * 1024, size_t completionCapacity = 0)
		: m_ring(capacity), m_completions(completionCapacity ? completionCapacity : capacity + 32), m_arena(arenaBytes)
	{
	}
	~Stream()
	{
		// Also safe during exception unwinding: failure/stop use out-of-band
		// channels and cannot be stuck behind a full operation/completion ring.
		RequestExit();
		if (m_worker.joinable())
			m_worker.join();
	}
	bool Recording() const
	{
		return s_recording == this;
	}
	bool Running() const
	{
		return m_running.load(std::memory_order_acquire);
	}
	void BindConsumerThread()
	{
		if (m_consuming.load())
			std::abort();
	}
	const Header &ActiveHeader() const
	{
		return m_activeHeader;
	}
	Arena &Staging()
	{
		return m_arena;
	}
	void Submitted(uint64_t sequence)
	{
		m_backTickets.Submit(sequence);
	}
	void SetCompletedFence(uint64_t fence)
	{
		m_backCompletedFence = fence;
	}
	uint64_t CompletedFence() const
	{
		return m_frontCompletedFence;
	}
	void Completed(uint64_t sequence)
	{
		m_backTickets.Complete(sequence);
	}
	const Tickets &Progress() const
	{
		return Recording() ? m_backTickets : m_frontTickets;
	}
	std::vector<ResourceId> &RetiredResources()
	{
		assert(Recording());
		return m_retiredResources;
	}
	Completion &Result()
	{
		assert(Recording());
		return m_result;
	}
	Completion TakeReply(uint64_t sequence)
	{
		if (m_reply.sequence != sequence)
			throw std::logic_error("Mismatched Latte reply sequence");
		return std::move(m_reply);
	}
	const PipelineTiming &Metrics() const
	{
		return m_frontPipelineTiming;
	}
	void Wake()
	{
		m_work.Signal();
	}
	uint64_t Occupancy() const
	{
		return m_ring.Size();
	}
	void Cancel()
	{
		Fail(std::make_exception_ptr(std::runtime_error("Latte recording cancelled")));
		RequestExit();
	}
	void SetCompletionHandler(std::function<void(const Completion &)> handler)
	{
		m_handler = std::move(handler);
	}

	template <class Owner, class Idle, class Begin>
	void Start(Context &context, Owner &owner, Idle idle, Begin begin, std::function<void()> end = {})
	{
		context.CheckFront();
		if (!context.Threaded() || Running())
			return;
		m_exit.store(false);
		m_threaded = true;
		m_running.store(true, std::memory_order_release);
		m_worker = std::thread([&, idle, begin, end] {
			try
			{
				context.BindBackThread();
				BindConsumerThread();
				begin();
				while (!m_exit.load(std::memory_order_acquire))
				{
					if (PauseWorker(context))
						continue;
					const auto epoch = m_work.Epoch();
					if (ConsumeOne(context, owner))
						continue;
					{
						NativeScope scope(*this, context);
						const auto start = m_measureRecording ? Clock::now() : Clock::time_point{};
						idle();
						if (m_measureRecording)
							m_backPipelineTiming.backBusyNs += Ns(start);
						PublishProgress();
					}
					if (m_ring.Empty() && !m_exit.load())
					{
						const auto start = Clock::now();
						m_work.Wait(epoch);
						m_backPipelineTiming.backIdleNs += Ns(start);
					}
				}
			}
			catch (...)
			{
				Fail(std::current_exception());
			}
			if (end)
				end();
			{
				std::lock_guard lock(m_pauseMutex);
				m_running.store(false, std::memory_order_release);
				m_pauseCV.notify_all();
			}
			m_space.Signal();
		});
	}
	template <class T, auto Record, class Owner>
	uint64_t Execute(Context &context, Opcode opcode, const T &value, Owner &owner, bool reply = false)
	{
		static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= 208);
		context.CheckFront();
		if (Recording())
			throw std::logic_error("Publish from Latte native callback");
		if (m_threaded)
			Poll();
		else
			CheckFailure();
		if (m_stopped)
			throw std::logic_error("Publish after Latte shutdown");
		const Header header{opcode, static_cast<uint16_t>(sizeof(T)), reply ? 1u : 0u, context.BeginOperation(), context.Frame()};
		auto prepare = [&](Packet &packet) {
			packet.header = header;
			packet.arenaEnd = m_arena.End();
			std::memcpy(packet.payload.data(), &value, sizeof(T));
			packet.record = [](void *target, const Packet &p, const Arena &arena) { (static_cast<Owner *>(target)->*Record)(p.As<T>(), arena); };
		};
		// Mode 1 consumes the same immutable packet immediately on this actor.
		// Ring cursor publication is useful only when another actor consumes it.
		if (m_threaded)
		{
			auto *slot = m_ring.TryWrite();
			if (!slot)
			{
				const auto start = Clock::now();
				do
				{
					const auto epoch = m_space.Epoch();
					Poll();
					CheckFailure();
					slot = m_ring.TryWrite();
					if (!slot)
						m_space.Wait(epoch);
				} while (!slot);
				m_frontPipelineTiming.backpressureNs += Ns(start);
			}
			prepare(*slot);
			const auto occupancy = m_ring.Size() + 1;
			m_ring.PublishWrite();
			m_lastPublished = header.sequence;
			m_frontPipelineTiming.highWater = std::max(m_frontPipelineTiming.highWater, occupancy);
			m_work.Signal();
		}
		else
		{
			// Preserve an immutable value while avoiding ring encoding, decoding
			// and indirect dispatch when both actors are this thread.
			const T snapshot = value;
			ConsumeOperation(context, header, m_arena.End(), [&] { (owner.*Record)(snapshot, m_arena); });
			if (m_handler || reply)
				Accept(std::move(m_result));
			else
				m_frontTickets = m_backTickets;
		}
		return header.sequence;
	}
	void Poll()
	{
		Completion completion;
		while (m_completions.Pop(completion))
		{
			++m_frontPipelineTiming.completionMessages;
			Accept(std::move(completion));
			m_space.Signal();
		}
		CheckFailure();
	}
	void Wait(uint64_t sequence)
	{
		WaitFor(sequence, false);
	}
	void WaitCapacity(uint64_t sequence = 0)
	{
		WaitFor(sequence ? sequence : m_lastPublished, true);
	}
	void WaitFor(uint64_t sequence, bool capacity)
	{
		auto last = Clock::now();
		while (m_frontTickets.recorded < sequence)
		{
			const auto epoch = m_space.Epoch();
			const auto now = Clock::now();
			(capacity ? m_frontPipelineTiming.backpressureNs : m_frontPipelineTiming.replyWaitNs) += std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count();
			last = now;
			Poll();
			if (m_frontTickets.recorded < sequence)
				m_space.Wait(epoch);
		}
		(capacity ? m_frontPipelineTiming.backpressureNs : m_frontPipelineTiming.replyWaitNs) += Ns(last);
	}
	void Finish()
	{
		if (m_lastPublished)
			Wait(m_lastPublished);
	}
	template <class Owner>
	void Drain(Context &context, Owner &owner)
	{
		context.CheckBack();
		while (ConsumeOne(context, owner))
		{
		}
		if (m_threaded)
			Poll();
	}
	template <class Owner>
	void Stop(Context &context, Owner &owner)
	{
		context.CheckFront();
		if (m_worker.joinable())
		{
			try
			{
				Finish();
			}
			catch (...)
			{
				RequestExit();
				m_worker.join();
				context.BindBackThread();
				throw;
			}
			RequestExit();
			m_worker.join();
			context.BindBackThread();
			BindConsumerThread();
			Poll();
		}
		else
			Drain(context, owner);
		m_stopped = true;
	}

	// Rare synchronous native handoff. The worker consumes the published prefix,
	// then parks. Producers may fill the bounded ring but cannot record. This is
	// also the CoreWindow/UI-thread creation contract; it never adds a producer.
	template <class F>
	auto Exclusive(Context &context, F function) -> decltype(function())
	{
		std::unique_lock serial(m_handoffMutex);
		if (!Running())
		{
			CheckFailure();
			NativeScope scope(*this, context);
			return function();
		}
		{
			std::unique_lock lock(m_pauseMutex);
			m_pauseRequested = true;
			m_pauseTarget = m_ring.Published();
			m_work.Signal();
			m_pauseCV.wait(lock, [&] { return m_paused || !Running(); });
		}
		CheckFailure();
		context.BindBackThread();
		auto resume = [&] {
			std::unique_lock lock(m_pauseMutex);
			m_pauseRequested = false;
			m_pauseCV.notify_all();
			m_pauseCV.wait(lock, [&] { return !m_paused || !Running(); });
		};
		try
		{
			if constexpr (std::is_void_v<decltype(function())>)
			{
				{
					NativeScope scope(*this, context);
					function();
				}
				resume();
			}
			else
			{
				auto value = [&] { NativeScope scope(*this, context); return function(); }();
				resume();
				return value;
			}
		}
		catch (...)
		{
			const auto error = std::current_exception();
			resume();
			Fail(error);
			RequestExit();
			throw;
		}
	}

  private:
	using Clock = std::chrono::steady_clock;
	static uint64_t Ns(Clock::time_point start)
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
	}
	class NativeScope
	{
	  public:
		NativeScope(Stream &stream, Context &context) : s(stream), c(context)
		{
			c.CheckBack();
			if (s.m_consuming.load(std::memory_order_relaxed))
				throw std::logic_error("Reentrant Latte recorder");
			s.m_consuming.store(true, std::memory_order_release);
			s_recording = &s;
			c.EnterRecording();
		}
		~NativeScope()
		{
			c.LeaveRecording();
			s_recording = nullptr;
			s.m_consuming.store(false, std::memory_order_release);
		}

	  private:
		Stream &s;
		Context &c;
	};
	bool PauseWorker(Context &context)
	{
		if (!m_pauseRequested.load(std::memory_order_acquire))
			return false;
		std::unique_lock lock(m_pauseMutex);
		if (!m_pauseRequested || m_ring.Consumed() < m_pauseTarget)
			return false;
		m_paused = true;
		m_pauseCV.notify_all();
		m_pauseCV.wait(lock, [&] { return !m_pauseRequested; });
		context.BindBackThread();
		m_paused = false;
		m_pauseCV.notify_all();
		return true;
	}
	void RequestExit()
	{
		m_exit.store(true, std::memory_order_release);
		m_work.Signal();
		m_space.Signal();
		std::lock_guard lock(m_pauseMutex);
		m_pauseCV.notify_all();
	}
	void Fail(std::exception_ptr error)
	{
		{
			std::lock_guard lock(m_failureMutex);
			m_failure = error;
		}
		m_failed.store(true, std::memory_order_release);
		m_space.Signal();
		m_work.Signal();
	}
	void CheckFailure() const
	{
		if (m_failed.load(std::memory_order_acquire))
		{
			std::lock_guard lock(m_failureMutex);
			std::rethrow_exception(m_failure);
		}
	}
	void Accept(Completion &&completion)
	{
		m_frontTickets = completion.tickets;
		m_frontCompletedFence = completion.completedFence;
		m_frontPipelineTiming.backBusyNs = completion.backBusyNs;
		m_frontPipelineTiming.backIdleNs = completion.backIdleNs;
		if (m_handler && (completion.kind != UINT32_MAX || !completion.retiredResources.empty()))
			m_handler(completion);
		if (completion.replyRequested)
			m_reply = std::move(completion);
	}
	void PublishProgress()
	{
		if (m_backTickets.recorded == m_notifiedTickets.recorded && m_backTickets.submitted == m_notifiedTickets.submitted && m_backTickets.completed == m_notifiedTickets.completed && m_backCompletedFence == m_notifiedFence && m_retiredResources.empty())
			return;
		// Idle fence progress is coalescible. Never block a UI handoff on this
		// informational message when the front is paused/not polling.
		if (auto *slot = m_completions.TryWrite())
		{
			*slot = {};
			slot->kind = UINT32_MAX;
			slot->tickets = m_backTickets;
			slot->retiredResources = std::move(m_retiredResources);
			slot->completedFence = m_backCompletedFence;
			m_notifiedFence = m_backCompletedFence;
			slot->backBusyNs = m_backPipelineTiming.backBusyNs;
			slot->backIdleNs = m_backPipelineTiming.backIdleNs;
			m_notifiedTickets = m_backTickets;
			m_completions.PublishWrite();
			m_unreported = 0;
			m_space.Signal();
		}
	}
	template <class Owner>
	bool ConsumeOne(Context &context, Owner &owner)
	{
		const auto *packet = m_ring.Peek();
		if (!packet)
			return false;
		Consume(context, *packet, owner);
		const bool deliver = m_handler || (packet->header.reserved & 1);
		m_ring.ReleaseRead();
		if (m_threaded)
		{
			// Ordinary operations only need a monotonic consumed prefix. Keep
			// data-bearing results lossless, and publish progress in batches or
			// at an empty queue so Finish/capacity waits cannot strand a tail.
			const bool result = m_result.replyRequested || m_result.kind || !m_result.bytes.empty() ||
								!m_result.retiredResources.empty() || std::any_of(m_result.words.begin(), m_result.words.end(), [](uint64_t word) { return word != 0; });
			if (!result && ++m_unreported < 32 && !m_ring.Empty())
				return true;
			while (!m_completions.TryWrite())
			{
				if (m_exit.load())
					return true;
				const auto epoch = m_space.Epoch();
				if (!m_completions.TryWrite())
					m_space.Wait(epoch);
			}
			if (!result)
				m_result.kind = UINT32_MAX;
			m_notifiedTickets = m_backTickets;
			m_notifiedFence = m_backCompletedFence;
			m_unreported = 0;
			*m_completions.TryWrite() = std::move(m_result);
			m_completions.PublishWrite();
			m_space.Signal();
		}
		else if (deliver)
			Accept(std::move(m_result));
		else
			m_frontTickets = m_backTickets;
		return true;
	}
	template <class Owner>
	void Consume(Context &context, const Packet &packet, Owner &owner)
	{
		ConsumeOperation(context, packet.header, packet.arenaEnd, [&] { packet.record(&owner, packet, m_arena); });
	}
	template <class Record>
	void ConsumeOperation(Context &context, const Header &header, uint64_t arenaEnd, Record record)
	{
		const auto start = m_threaded && m_measureRecording ? Clock::now() : Clock::time_point{};
		NativeScope scope(*this, context);
		m_activeHeader = header;
		const bool resultNeeded = m_threaded || m_handler || (header.reserved & 1);
		if (resultNeeded)
			m_result = {};
		record();
		m_backTickets.Record(header.sequence);
		m_arena.Retire(arenaEnd);
		if (resultNeeded)
		{
			m_result.retiredResources = std::move(m_retiredResources);
			m_result.replyRequested = (header.reserved & 1) != 0;
			m_result.sequence = header.sequence;
			m_result.completedFence = m_backCompletedFence;
			m_result.tickets = m_backTickets;
			if (m_threaded && m_measureRecording)
				m_backPipelineTiming.backBusyNs += Ns(start);
			m_result.backBusyNs = m_backPipelineTiming.backBusyNs;
			m_result.backIdleNs = m_backPipelineTiming.backIdleNs;
		}
	}
	Ring<Packet> m_ring;
	Ring<Completion> m_completions;
	Arena m_arena;
	Tickets m_backTickets, m_frontTickets, m_notifiedTickets;
	uint64_t m_backCompletedFence = 0, m_frontCompletedFence = 0, m_notifiedFence = 0;
	Completion m_result, m_reply;
	std::vector<ResourceId> m_retiredResources;
	Header m_activeHeader{};
	PipelineTiming m_frontPipelineTiming, m_backPipelineTiming;
	std::function<void(const Completion &)> m_handler;
	inline static thread_local Stream *s_recording = nullptr;
	std::atomic<bool> m_consuming{false}, m_running{false}, m_exit{false}, m_failed{false};
	Event m_work, m_space;
	std::thread m_worker;
	mutable std::mutex m_failureMutex;
	std::exception_ptr m_failure;
	std::mutex m_handoffMutex, m_pauseMutex;
	std::condition_variable m_pauseCV;
	std::atomic<bool> m_pauseRequested{false};
	bool m_paused = false;
	uint64_t m_pauseTarget = 0;
	unsigned m_unreported = 0;
	uint64_t m_lastPublished = 0;
	// Per-operation clock reads are diagnostics too. Keep the release worker's
	// normal path free of them when the Latte profiler is disabled.
	const bool m_measureRecording = EnvironmentFlag("CEMU_LATTE_PROFILE");
	bool m_stopped = false, m_threaded = false;
};
} // namespace LattePipeline
