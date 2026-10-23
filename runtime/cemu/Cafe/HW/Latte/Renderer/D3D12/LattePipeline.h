#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

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
		return m_queued;
	}
	void CheckFront() const
	{
		Check(m_front, "front");
#if defined(NDEBUG) && !defined(_DEBUG)
		if (!m_checks)
			return;
#endif
		if (m_backActive.load(std::memory_order_acquire) && m_back == std::this_thread::get_id())
		{
			std::fputs("Latte pipeline: front access from recorder\n", stderr);
			std::abort();
		}
	}
	void CheckBack() const
	{
		Check(m_back, "back");
	}
	// Ownership handoff is only legal at a drained lifecycle boundary. Phase 3
	// leaves both owners on the command thread; no worker is started here.
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
			m_backActive.store(true, std::memory_order_release);
	}
	void LeaveRecording()
	{
		if (ChecksEnabled())
			m_backActive.store(false, std::memory_order_release);
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
	const std::thread::id m_front;
	std::thread::id m_back;
	const bool m_checks = EnvironmentFlag("CEMU_LATTE_PIPELINE_CHECKS");
	const bool m_queued = EnvironmentFlag("CEMU_LATTE_PIPELINE");
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
		m_entries[id.slot].retiring = true;
	}
	void Retire(uint64_t consumed, uint64_t completed)
	{
		std::lock_guard lock(m_allocationMutex);
		const auto count = m_count.load(std::memory_order_acquire);
		for (uint32_t i = 0; i < count; ++i)
		{
			auto &e = m_entries[i];
			if (!e.retiring || !e.object || e.lastSequence > consumed || e.lastFence > completed)
				continue;
			e.object = nullptr;
			e.retiring = false;
			// Never wrap a generation into an old valid ID.
			if (e.generation != std::numeric_limits<uint32_t>::max())
			{
				++e.generation;
				m_free.push_back(i);
			}
		}
	}

  private:
	const uint32_t m_capacity;
	std::unique_ptr<Entry[]> m_entries;
	std::atomic<uint32_t> m_count{0};
	std::vector<uint32_t> m_free;
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
		for (size_t i = 0; i < Capacity; ++i)
		{
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
		value = *slot;
		ReleaseRead();
		return true;
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

// Phase 3: a synchronous consumer. There is deliberately no worker, condition
// variable or implicit CPU/GPU wait. Phase 4 replaces Drain's caller and adds
// completion transport without changing packet contents or SPSC ordering.
class Stream
{
  public:
	Stream(size_t capacity = 1024, size_t arenaBytes = 16 * 1024 * 1024) : m_ring(capacity), m_arena(arenaBytes)
	{
	}
	bool Recording() const
	{
		return m_consuming.load(std::memory_order_acquire) && m_consumerOwner == std::this_thread::get_id();
	}
	void BindConsumerThread()
	{
		if (m_consuming.load(std::memory_order_acquire))
			std::abort();
		m_consumerOwner = std::this_thread::get_id();
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
		m_tickets.Submit(sequence);
	}
	void Completed(uint64_t sequence)
	{
		m_tickets.Complete(sequence);
	}
	const Tickets &Progress() const
	{
		return m_tickets;
	}
	template <class T, auto Record, class Owner>
	void Execute(Context &context, Opcode opcode, const T &value, Owner &owner)
	{
		static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= 208);
		context.CheckFront();
		if (m_stopped)
			throw std::logic_error("Publish after Latte shutdown");
		const Header header{opcode, static_cast<uint16_t>(sizeof(T)), 0, context.BeginOperation(), context.Frame()};
		auto prepare = [&](Packet &packet) {
			packet.header = header;
			packet.arenaEnd = m_arena.End();
			std::memcpy(packet.payload.data(), &value, sizeof(T));
			packet.record = [](void *target, const Packet &p, const Arena &arena) { (static_cast<Owner *>(target)->*Record)(p.As<T>(), arena); };
		};
		if (context.Queued())
		{
			auto *slot = m_ring.TryWrite();
			while (!slot)
			{
				Drain(context, owner);
				slot = m_ring.TryWrite();
			}
			prepare(*slot);
			m_ring.PublishWrite();
			Drain(context, owner);
		}
		else
		{
			Packet packet;
			prepare(packet);
			Consume(context, packet, owner);
		}
	}

	template <class Owner>
	void Drain(Context &context, Owner &owner)
	{
		context.CheckBack();
		while (const auto *packet = m_ring.Peek())
		{
			try
			{
				Consume(context, *packet, owner);
			}
			catch (...)
			{
				m_ring.ReleaseRead();
				throw;
			}
			m_ring.ReleaseRead();
		}
	}
	template <class Owner>
	void Stop(Context &context, Owner &owner)
	{
		Drain(context, owner);
		m_stopped = true;
	}

  private:
	template <class Owner>
	void Consume(Context &context, const Packet &packet, Owner &owner)
	{
		context.CheckBack();
		// Reentrant guest callbacks must not publish while an immutable packet is
		// being recorded. Completion application belongs on front after recording.
		if (m_consuming.load(std::memory_order_relaxed))
			throw std::logic_error("Reentrant Latte recorder");
		m_consuming.store(true, std::memory_order_release);
		m_activeHeader = packet.header;
		context.EnterRecording();
		try
		{
			packet.record(&owner, packet, m_arena);
		}
		catch (...)
		{
			context.LeaveRecording();
			m_consuming.store(false, std::memory_order_release);
			throw;
		}
		context.LeaveRecording();
		m_consuming.store(false, std::memory_order_release);
		m_tickets.Record(packet.header.sequence);
		m_arena.Retire(packet.arenaEnd);
	}
	Ring<Packet> m_ring;
	Arena m_arena;
	Tickets m_tickets;
	Header m_activeHeader{};
	std::thread::id m_consumerOwner = std::this_thread::get_id();
	std::atomic<bool> m_consuming{false};
	bool m_stopped = false;
};
} // namespace LattePipeline
