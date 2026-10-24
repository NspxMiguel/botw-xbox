// Production scheduler and descriptor hashing with deterministic captured
// input. This measures host CPU preparation, not Xbox driver time or frame
// rate.
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12StageTable.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteChunkWorkers.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>

struct Input {
  D3D12StageTableDesc stages[3];
  std::array<std::byte, 3072> uniforms;
  uint32_t indexCount;
};
struct Output {
  std::array<std::byte, 3072> upload;
  std::array<uint64_t, 96> commands;
  bool operator==(const Output &) const = default;
};
static void Record(const Input &input, Output &output) {
  std::copy(input.uniforms.begin(), input.uniforms.end(),
            output.upload.begin());
  for (size_t i = 0; i < output.commands.size(); ++i) {
    const auto &stage = input.stages[i % 3];
    output.commands[i] =
        stage.TableKey(static_cast<D3D12Const::Stage>(i % 3)).Hash() ^
        (uint64_t(input.indexCount) << 32) ^ i;
  }
}
int main() {
  constexpr size_t draws = 5000;
  std::vector<Input> inputs(draws);
  for (size_t i = 0; i < draws; ++i) {
    auto &draw = inputs[i];
    draw.indexCount = 100 + i;
    draw.uniforms.fill(static_cast<std::byte>(i));
    for (auto &stage : draw.stages)
      for (unsigned j = 0; j < D3D12Const::kMaxSRVsPerStage; ++j)
        stage.srv[j].ptr = 0x100000 + (i * 18 + j) * 32;
  }
  std::vector<Output> reference(draws), outputs(draws);
  for (size_t i = 0; i < draws; ++i)
    Record(inputs[i], reference[i]);
  LattePipeline::ChunkWorkers workers;
  std::array<std::vector<double>, 4> windows;
  for (unsigned window = 0; window < 11; ++window)
    for (unsigned order = 0; order < 4; ++order) {
      const auto k = window % 2 ? 4 - order : order + 1;
      const auto start = std::chrono::steady_clock::now();
      for (unsigned frame = 0; frame < 20; ++frame)
        workers.Run(
            draws, k,
            [&](unsigned, LattePipeline::ChunkRange range) {
              for (size_t i = range.begin; i < range.end; ++i)
                Record(inputs[i], outputs[i]);
            },
            false);
      const auto ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count() /
                      20;
      if (window)
        windows[k - 1].push_back(ms);
      if (outputs != reference)
        throw std::runtime_error("Parallel recording benchmark parity failed");
    }
  double baseline = 0;
  for (unsigned k = 1; k <= 4; ++k) {
    auto &samples = windows[k - 1];
    std::sort(samples.begin(), samples.end());
    const double median = (samples[4] + samples[5]) / 2;
    if (k == 1)
      baseline = median;
    std::cout << std::fixed << std::setprecision(4) << "K=" << k << ": "
              << median << " ms/5000 draws; scaling " << baseline / median
              << "x (ideal " << k << "x)\n";
  }
}
