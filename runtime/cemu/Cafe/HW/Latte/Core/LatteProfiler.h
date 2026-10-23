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
	DrawStateDecode,
	PipelineBind,
	VertexBuffers,
	RootSignature,
	RenderState,
	DrawCall,
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
	DescriptorHits,
	DescriptorFastHits,
	DescriptorMisses,
	RootCBVSets,
	RootCBVSkips,
	DynamicStateHits,
	DynamicStateMisses,
	TransitionHits,
	TransitionMisses,
	IndexHits,
	IndexMisses,
	IndexContentMisses,
	IndexSetEvictions,
	IndexTransientMisses,
	FramebufferHits,
	FramebufferMisses,
	PipelineStateHits,
	Submissions,
	EmptySubmissions,
	IndexRelocatedHits,
	IndexComparedBytes,
	IndexHashedBytes,
	TextureStageHits,
	TextureStageMisses,
	TextureLookupHits,
	TextureLookupMisses,
	TextureContentChecks,
	TextureAliasRefreshes,
	DrawSequences,
	RegisterPackets,
	RegisterWords,
	RegisterChangedPackets,
	RingWords,
	RingRefills,
	SubmissionWaits,
	SubmissionDrawLimit,
	SubmissionReadback,
	SubmissionOverflow,
	Count,
};

enum class Detail : size_t
{
	Sequence,
	ShaderResolve,
	PacketDecode,
	Streamout,
	BindingsPrepare,
	SequenceState,
	SequenceFinish,
	DrawSetup,
	IndexLookup,
	IndexHit,
	IndexMiss,
	IndexConvert,
	TextureStage,
	TextureResolve,
	TextureContent,
	TextureAliases,
	Pm4Registers,
	SubmitClose,
	SubmitExecute,
	SubmitSignal,
	SubmitRetire,
	SubmitReset,
	SubmitWait,
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
	bool detailEnabled = false;
	std::array<int64_t, static_cast<size_t>(Detail::Count)> detailNs{};
	std::array<Clock::time_point, static_cast<size_t>(Detail::Count)> detailStart{};
	std::array<uint32_t, static_cast<size_t>(Detail::Count)> detailDepth{};
};

// Only the command processor enables its own state. Worker threads never contribute.
inline thread_local State s_state;

inline bool IsConfigured()
{
	static const bool enabled = [] {
		const char *value = std::getenv("CEMU_LATTE_PROFILE");
		// Off by default: ~20 timed slots per draw cost milliseconds per frame at 5k draws.
		return value && std::strcmp(value, "0") != 0;
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
		const char *detail = std::getenv("CEMU_LATTE_PROFILE_DETAIL");
		s_state.detailEnabled = s_state.enabled && detail && std::strcmp(detail, "0") != 0;
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

// Draw handlers enter their exclusive slot and the inclusive draw union at the
// same instant. Avoid a timed DrawOther wrapper with no work between the scopes.
class DrawScope
{
  public:
	explicit DrawScope(Slot slot) : m_enabled(s_state.enabled)
	{
		if (!m_enabled)
			return;
		Checkpoint(Clock::now());
		m_previous = s_state.current;
		s_state.current = slot;
		++s_state.drawDepth;
	}
	~DrawScope()
	{
		if (!m_enabled)
			return;
		Checkpoint(Clock::now());
		--s_state.drawDepth;
		s_state.current = m_previous;
	}
	DrawScope(const DrawScope &) = delete;
	DrawScope &operator=(const DrawScope &) = delete;

  private:
	Slot m_previous = Slot::Unaccounted;
	bool m_enabled;
};

// Inclusive diagnostic intervals, never added to coverage or the slot total.
class DetailScope
{
  public:
	explicit DetailScope(Detail detail) : m_detail(detail), m_enabled(s_state.enabled && s_state.detailEnabled)
	{
		if (m_enabled && s_state.detailDepth[static_cast<size_t>(m_detail)]++ == 0)
			s_state.detailStart[static_cast<size_t>(m_detail)] = Clock::now();
	}
	~DetailScope()
	{
		End(m_detail);
	}
	void End(Detail outcome)
	{
		const auto index = static_cast<size_t>(m_detail);
		if (m_enabled && --s_state.detailDepth[index] == 0)
			s_state.detailNs[static_cast<size_t>(outcome)] += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - s_state.detailStart[index]).count();
		m_enabled = false;
	}
	DetailScope(const DetailScope &) = delete;
	DetailScope &operator=(const DetailScope &) = delete;

  private:
	Detail m_detail;
	bool m_enabled;
};

inline void Add(Counter counter, uint64_t value = 1)
{
	if (s_state.enabled)
		s_state.counters[static_cast<size_t>(counter)] += value;
}

inline void CheckpointDetails(Clock::time_point now)
{
	for (size_t i = 0; i < s_state.detailNs.size(); ++i)
	{
		if (s_state.detailDepth[i])
		{
			s_state.detailNs[i] += std::chrono::duration_cast<std::chrono::nanoseconds>(now - s_state.detailStart[i]).count();
			s_state.detailStart[i] = now;
		}
	}
}

// Reset counters at a present boundary while preserving any live scopes.
inline void Reset(Clock::time_point now)
{
	if (!s_state.enabled)
		return;
	s_state.timeNs.fill(0);
	s_state.detailNs.fill(0);
	s_state.detailStart.fill(now);
	s_state.counters.fill(0);
	s_state.drawTotalNs = 0;
	s_state.last = now;
}

void Report(Clock::time_point now, double seconds, uint64_t frames);
} // namespace LatteProfiler
