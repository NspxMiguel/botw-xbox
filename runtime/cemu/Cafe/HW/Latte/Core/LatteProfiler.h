#pragma once

#include "Cafe/HW/Latte/Renderer/D3D12/LatteBatchPolicy.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace LatteProfiler {
using Clock = std::chrono::steady_clock;

enum class Slot : size_t {
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
  RootCBV,
  Barriers,
  DrawSetup,
  ResourcePin,
  BackendCreate,
  BackendRelease,
  BackendUpload,
  BackendCopy,
  BackendPresent,
  BackendQuery,
  BackendMaintenance,
  Transport,
  DescriptorPrepare,
  DescriptorSamplers,
  DescriptorLookup,
  DescriptorCopy,
  ReplyCreate,
  ReplyRelease,
  ReplyPresent,
  ReplySubmit,
  ReplyReadback,
  ReplyOther,
  BackendReadback,
  Count,
  Unaccounted = Count,
};

enum class Counter : size_t {
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
  SequenceEndings,
  ResourceCreateReplies,
  ForcedReadbacks,
  QueryWaits,
  UploadBytes,
  IndexUploadBytes,
  UniformUploadBytes,
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
  DrawBlockReuses,
  IndexAddressComparedBytes,
  IndexRelocatedComparedBytes,
  IndexSampleRejects,
  AsyncSubmissions,
  ParallelDraws,
  SerialDraws,
  AsyncControls,
  DeferredIndexBytes,
  ChunkFallbackStreamout,
  ChunkFallbackQuery,
  ChunkFallbackAlignment,
  ChunkFallbackSlice,
  ChunkFallbackSimulation,
  Count,
};

enum class Detail : size_t {
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

struct State {
  std::array<int64_t, static_cast<size_t>(Slot::Count)> timeNs{};
  std::array<int64_t, static_cast<size_t>(Slot::Count)> backTimeNs{};
  std::array<uint64_t, static_cast<size_t>(Counter::Count)> counters{};
  Clock::time_point last{};
  int64_t drawTotalNs = 0;
  Slot current = Slot::Unaccounted;
  uint32_t drawDepth = 0;
  uint64_t operationSequence = 0, sourceFrame = 0, recordedSequence = 0,
           submittedSequence = 0, completedSequence = 0;
  uint64_t frontBusyNs = 0, backBusyNs = 0, backRecordWallNs = 0,
           frontStallNs = 0, frontReplyNs = 0, backIdleNs = 0,
           ringHighWater = 0;
  uint64_t frontCpuNs = 0, backCpuNs = 0;
  std::array<uint64_t, 4> chunkBusyNs{};
  uint64_t chunkCriticalPathNs = 0, chunkCount = 0, chunkBatches = 0;
  bool threaded = false;
  static constexpr size_t backendKinds =
      static_cast<size_t>(LattePipeline::BackendKind::Count);
  std::array<uint64_t, backendKinds> replyCalls{}, replyWaitNs{}, chunkDrains{};
  bool enabled = false;
  bool detailEnabled = false;
  std::array<int64_t, static_cast<size_t>(Detail::Count)> detailNs{};
  std::array<Clock::time_point, static_cast<size_t>(Detail::Count)>
      detailStart{};
  std::array<uint32_t, static_cast<size_t>(Detail::Count)> detailDepth{};
};

// Each actor owns a thread-local collector. Worker snapshots cross the
// completion ring.
inline thread_local State s_state;

inline bool IsConfigured() {
  static const bool enabled = [] {
    const char *value = std::getenv("CEMU_LATTE_PROFILE");
    // Off by default: ~20 timed slots per draw cost milliseconds per frame at
    // 5k draws.
    return value && std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

inline void Checkpoint(Clock::time_point now) {
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - s_state.last)
          .count();
  if (s_state.current != Slot::Unaccounted)
    s_state.timeNs[static_cast<size_t>(s_state.current)] += ns;
  if (s_state.drawDepth)
    s_state.drawTotalNs += ns;
  s_state.last = now;
}

class ThreadScope {
public:
  ThreadScope() {
    s_state = {};
    s_state.enabled = IsConfigured();
    const char *detail = std::getenv("CEMU_LATTE_PROFILE_DETAIL");
    s_state.detailEnabled =
        s_state.enabled && detail && std::strcmp(detail, "0") != 0;
    if (s_state.enabled) {
      s_state.last = Clock::now();
      s_state.current = Slot::Pm4;
    }
  }
  ~ThreadScope() { s_state.enabled = false; }
  ThreadScope(const ThreadScope &) = delete;
  ThreadScope &operator=(const ThreadScope &) = delete;
};

// Each elapsed interval belongs to the innermost slot. Same-slot nesting needs
// no clock reads. DrawOther additionally tracks inclusive draw time across
// nested stages, without double counting.
class Scope {
public:
  explicit Scope(Slot slot, bool active = true)
      : m_slot(slot),
        m_enabled(active && s_state.enabled && s_state.current != slot) {
    if (!m_enabled)
      return;
    Checkpoint(Clock::now());
    m_previous = s_state.current;
    s_state.current = slot;
    if (slot == Slot::DrawOther)
      ++s_state.drawDepth;
  }
  ~Scope() {
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
// same instant. Avoid a timed DrawOther wrapper with no work between the
// scopes.
class DrawScope {
public:
  explicit DrawScope(Slot slot) : m_enabled(s_state.enabled) {
    if (!m_enabled)
      return;
    Checkpoint(Clock::now());
    m_previous = s_state.current;
    s_state.current = slot;
    ++s_state.drawDepth;
  }
  ~DrawScope() {
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
class DetailScope {
public:
  explicit DetailScope(Detail detail)
      : m_detail(detail), m_enabled(s_state.enabled && s_state.detailEnabled) {
    if (m_enabled && s_state.detailDepth[static_cast<size_t>(m_detail)]++ == 0)
      s_state.detailStart[static_cast<size_t>(m_detail)] = Clock::now();
  }
  ~DetailScope() { End(m_detail); }
  void End(Detail outcome) {
    const auto index = static_cast<size_t>(m_detail);
    if (m_enabled && --s_state.detailDepth[index] == 0)
      s_state.detailNs[static_cast<size_t>(outcome)] +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              Clock::now() - s_state.detailStart[index])
              .count();
    m_enabled = false;
  }
  DetailScope(const DetailScope &) = delete;
  DetailScope &operator=(const DetailScope &) = delete;

private:
  Detail m_detail;
  bool m_enabled;
};

inline void Add(Counter counter, uint64_t value = 1) {
  if (s_state.enabled)
    s_state.counters[static_cast<size_t>(counter)] += value;
}

inline void CheckpointDetails(Clock::time_point now) {
  for (size_t i = 0; i < s_state.detailNs.size(); ++i) {
    if (s_state.detailDepth[i]) {
      s_state.detailNs[i] +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              now - s_state.detailStart[i])
              .count();
      s_state.detailStart[i] = now;
    }
  }
}

// Only the front accepts a completed worker window. Never fold worker wall
// slots into front coverage: both threads run during the same interval.
inline void AcceptBackProfile(const State &back) {
  s_state.backTimeNs = back.timeNs;
  s_state.chunkDrains = back.chunkDrains;
  s_state.chunkBusyNs = back.chunkBusyNs;
  s_state.chunkCriticalPathNs = back.chunkCriticalPathNs;
  s_state.chunkCount = back.chunkCount;
  s_state.chunkBatches = back.chunkBatches;
  for (size_t i = 0; i < s_state.counters.size(); ++i)
    s_state.counters[i] += back.counters[i];
}

// Reset counters at a present boundary while preserving any live scopes.
inline void Reset(Clock::time_point now) {
  if (!s_state.enabled)
    return;
  s_state.timeNs.fill(0);
  s_state.backTimeNs.fill(0);
  s_state.detailNs.fill(0);
  s_state.detailStart.fill(now);
  s_state.counters.fill(0);
  s_state.replyCalls.fill(0);
  s_state.replyWaitNs.fill(0);
  s_state.chunkDrains.fill(0);
  s_state.chunkBusyNs.fill(0);
  s_state.chunkCriticalPathNs = s_state.chunkCount = s_state.chunkBatches = 0;
  s_state.drawTotalNs = 0;
  s_state.last = now;
}

void Report(Clock::time_point now, double seconds, uint64_t frames);
} // namespace LatteProfiler
