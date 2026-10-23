#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace LatteProfiler
{
using Clock = std::chrono::steady_clock;

enum class Slot : size_t
{
	DrawOther,
	ShaderPipeline,
	TextureBinding,
	TextureLoad,
	BufferSync,
	Indices,
	Uniforms,
	Descriptors,
	RenderTargets,
	Submit,
	Pm4,
	Count,
	Unaccounted = Count,
};

enum class Counter : size_t
{
	Draws,
	PipelineMisses,
	TextureMisses,
	TextureBytes,
	BufferBytes,
	Count,
};

struct State
{
	std::array<int64_t, static_cast<size_t>(Slot::Count)> timeNs{};
	std::array<uint64_t, static_cast<size_t>(Counter::Count)> counters{};
	Clock::time_point last{};
	int64_t drawTotalNs = 0;
	Slot current = Slot::Unaccounted;
	uint32_t drawDepth = 0;
	bool enabled = false;
};

// Only the command processor enables its own state. Worker threads never contribute.
inline thread_local State s_state;

inline bool IsConfigured()
{
	static const bool enabled = [] {
		const char *value = std::getenv("CEMU_LATTE_PROFILE");
#ifdef BOTWRECOMP
		return !value || std::strcmp(value, "0") != 0;
#else
		return value && std::strcmp(value, "0") != 0;
#endif
	}();
	return enabled;
}

inline void Checkpoint(Clock::time_point now)
{
	const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - s_state.last).count();
	if (s_state.current != Slot::Unaccounted)
		s_state.timeNs[static_cast<size_t>(s_state.current)] += ns;
	if (s_state.drawDepth)
		s_state.drawTotalNs += ns;
	s_state.last = now;
}

class ThreadScope
{
  public:
	ThreadScope()
	{
		s_state = {};
		s_state.enabled = IsConfigured();
		if (s_state.enabled)
		{
			s_state.last = Clock::now();
			s_state.current = Slot::Pm4;
		}
	}
	~ThreadScope()
	{
		s_state.enabled = false;
	}
	ThreadScope(const ThreadScope &) = delete;
	ThreadScope &operator=(const ThreadScope &) = delete;
};

// Each elapsed interval belongs to the innermost slot. Same-slot nesting needs no clock reads.
// DrawOther additionally tracks inclusive draw time across nested stages, without double counting.
class Scope
{
  public:
	explicit Scope(Slot slot) : m_slot(slot), m_enabled(s_state.enabled && s_state.current != slot)
	{
		if (!m_enabled)
			return;
		Checkpoint(Clock::now());
		m_previous = s_state.current;
		s_state.current = slot;
		if (slot == Slot::DrawOther)
			++s_state.drawDepth;
	}
	~Scope()
	{
		if (!m_enabled)
			return;
		Checkpoint(Clock::now());
		if (m_slot == Slot::DrawOther)
			--s_state.drawDepth;
		s_state.current = m_previous;
	}
	Scope(const Scope &) = delete;
	Scope &operator=(const Scope &) = delete;

  private:
	Slot m_slot;
	Slot m_previous = Slot::Unaccounted;
	bool m_enabled;
};

inline void Add(Counter counter, uint64_t value = 1)
{
	if (s_state.enabled)
		s_state.counters[static_cast<size_t>(counter)] += value;
}

// Reset counters at a present boundary while preserving any live scopes.
inline void Reset(Clock::time_point now)
{
	if (!s_state.enabled)
		return;
	s_state.timeNs.fill(0);
	s_state.counters.fill(0);
	s_state.drawTotalNs = 0;
	s_state.last = now;
}

void Report(Clock::time_point now, double seconds, uint64_t frames);
} // namespace LatteProfiler
