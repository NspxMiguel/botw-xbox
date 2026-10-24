// Replay console-shaped state/retirement traffic through the production batch
// policy and scheduler. Host CPU timings exclude the Xbox driver and GPU.
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ParallelRecorder.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteBatchPolicy.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>

using Kind = LattePipeline::BackendKind;
struct Input {
  std::array<D3D12StageTableDesc, 3> stages{};
  std::array<std::byte, 448> uniforms{};
  uint64 target = 0;
};
struct Output {
  std::array<std::byte, 448> upload{};
  std::array<uint64, 3> tables{};
  uint64 target = 0;
  bool operator==(const Output &) const = default;
};
static void Record(const Input &input, Output &out) {
  out.upload = input.uniforms;
  out.target = input.target;
  for (unsigned stage = 0; stage < 3; ++stage)
    out.tables[stage] = input.stages[stage]
                            .TableKey(static_cast<D3D12Const::Stage>(stage))
                            .Hash();
}
struct Stats {
  size_t batches = 0, chunks = 0;
  std::array<size_t, 4> busyDraws{};
};
int main() {
  // True command/lifetime dependencies must retain ordered drains.
  for (auto kind : {Kind::BufferUpload, Kind::IndexUpload, Kind::TexturePlane,
                    Kind::ViewRelease, Kind::TextureRelease, Kind::SurfaceCopy,
                    Kind::texture_clearColorSlice, Kind::BindUniform})
    if (!LattePipeline::DrainsDrawBatch(kind))
      throw std::runtime_error("Lost a native dependency boundary");
  if (LattePipeline::DrainsDrawBatch(Kind::EndSequence) ||
      !LattePipeline::DrainsDrawBatch(Kind::EndSequence, true) ||
      LattePipeline::DrainsDrawBatch(Kind::ServicePresent) ||
      !LattePipeline::DrainsDrawBatch(Kind::ServicePresent, false, true))
    throw std::runtime_error("Incorrect conditional dependency boundary");
  constexpr size_t drawCount = 5000, segments = 470;
  std::vector<Input> inputs(drawCount);
  for (size_t segment = 0; segment < segments; ++segment)
    for (size_t i = drawCount * segment / segments;
         i < drawCount * (segment + 1) / segments; ++i) {
      auto &input = inputs[i];
      input.target = segment % 17;
      input.uniforms.fill(static_cast<std::byte>(i));
      for (auto &stage : input.stages)
        for (unsigned reg = 0; reg < D3D12Const::kMaxSRVsPerStage; ++reg)
          stage.srv[reg].ptr = 0x10000 + (segment * 18 + reg) * 32;
    }
  std::vector<Output> reference(drawCount), output(drawCount);
  for (size_t i = 0; i < drawCount; ++i)
    Record(inputs[i], reference[i]);
  LattePipeline::ChunkWorkers workers;
  // Equal work per draw in every mode, with immutable capture charged to
  // mode 3.
  std::vector<Input> captured;
  captured.reserve(LattePipeline::ChunkWorkers::MaxDraws);
  auto frame = [&](unsigned mode, unsigned requested) {
    Stats stats;
    size_t first = 0, bytes = 0;
    auto drain = [&] {
      if (captured.empty())
        return;
      const auto timing = workers.Run(
          captured.size(), requested,
          [&](unsigned worker, LattePipeline::ChunkRange range) {
            for (size_t i = range.begin; i < range.end; ++i)
              Record(captured[i], output[first + i]);
            stats.busyDraws[worker] += range.end - range.begin;
          },
          false);
      ++stats.batches;
      stats.chunks += timing.count;
      first += captured.size();
      captured.clear();
      bytes = 0;
    };
    for (size_t segment = 0; segment < segments; ++segment) {
      // A target/viewport/sampler change, an index eviction, and a query poll.
      // Phase 7 drained on the latter two despite their not emitting commands.
      for (auto kind : {Kind::BindFramebuffer, Kind::Viewport,
                        Kind::IndexRelease, Kind::occlusionQuery_updateState}) {
        const bool preserves =
            mode == 7 ? kind == Kind::BindFramebuffer || kind == Kind::Viewport
                      : !LattePipeline::DrainsDrawBatch(kind);
        if (mode != 2 && !preserves)
          drain();
      }
      for (size_t i = drawCount * segment / segments;
           i < drawCount * (segment + 1) / segments; ++i) {
        if (mode == 2) {
          Record(inputs[i], output[i]);
          continue;
        }
        const size_t limit = LattePipeline::ChunkWorkers::DrawsPerChunk *
                             (requested ? requested : 4);
        const size_t drawBytes =
            sizeof(D3D12ResolvedDraw) + inputs[i].uniforms.size() + 64;
        if (captured.size() == limit || bytes + drawBytes > 16 * 1024 * 1024)
          drain();
        captured.push_back(inputs[i]);
        bytes += drawBytes;
      }
    }
    drain();
    if (output != reference)
      throw std::runtime_error("Segment benchmark per-draw parity failed");
    return stats;
  };
  std::array<std::vector<double>, 6> timings;
  std::array<Stats, 6> stats;
  for (unsigned window = 0; window < 10; ++window)
    for (unsigned order = 0; order < 6; ++order) {
      const unsigned test = window % 2 ? 5 - order : order;
      const auto start = std::chrono::steady_clock::now();
      for (unsigned f = 0; f < 20; ++f)
        stats[test] = frame(test == 0   ? 2
                            : test == 1 ? 7
                                        : 3,
                            test < 2 ? 0 : test - 1);
      const double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start)
                            .count() /
                        20;
      if (window)
        timings[test].push_back(ms);
    }
  const auto automatic = frame(3, 0);
  if (automatic.batches != 3 || automatic.chunks != 10 ||
      automatic.busyDraws[3] == 0)
    throw std::runtime_error("Automatic workers failed console-shaped replay");
  double baseline = 0;
  for (unsigned test = 0; test < 6; ++test) {
    auto &samples = timings[test];
    std::sort(samples.begin(), samples.end());
    const double median = samples[samples.size() / 2];
    if (!test)
      baseline = median;
    const auto &s = stats[test];
    if (test == 1 &&
        (s.batches != 470 || s.chunks != 470 || s.busyDraws[0] != 5000))
      throw std::runtime_error("Failed to reproduce console batch topology");
    if (test == 5 && (s.batches != 3 || s.chunks != 12 || s.busyDraws[3] == 0))
      throw std::runtime_error("Fixed state traffic fragmented the frame");
    std::cout << std::fixed << std::setprecision(4)
              << (test == 0   ? "mode2"
                  : test == 1 ? "phase7"
                              : "mode3")
              << " K=" << (test < 2 ? 0 : test - 1) << " " << median
              << " ms; mode2/time=" << baseline / median
              << "; batches/chunks=" << s.batches << "/" << s.chunks
              << "; worker draws=";
    for (auto draws : s.busyDraws)
      std::cout << draws << ' ';
    std::cout << '\n';
  }
}
