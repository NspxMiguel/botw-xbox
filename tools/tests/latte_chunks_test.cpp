#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteChunkWorkers.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LattePipeline.h"
namespace Latte {
enum class E_DIM : uint32_t { DIM_2D };
}
#define LATTE_MAX_VERTEX_BUFFERS 16
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DrawPacket.h"
#include <iostream>

using namespace LattePipeline;

static std::vector<uint64_t>
Emit(const DrawPacket &packet, const DrawBlock &block,
     const std::array<std::vector<std::byte>, 3> &uniforms,
     const std::vector<std::byte> &runtime) {
  std::vector<uint64_t> result;
  auto bytes = [&](auto data) {
    for (auto byte : std::as_bytes(data))
      result.push_back(std::to_integer<uint8_t>(byte));
  };
  for (auto id : {packet.pipeline, packet.framebuffer, packet.indices}) {
    result.push_back(id.slot);
    result.push_back(id.generation);
  }
  for (auto value :
       {packet.baseVertex, packet.baseInstance, packet.instanceCount,
        packet.count, packet.indexCount, packet.indexType, packet.stencilRef,
        static_cast<uint32_t>(packet.streamout)})
    result.push_back(value);
  bytes(std::span{&packet.viewport, 1});
  bytes(std::span{&packet.scissor, 1});
  bytes(std::span{packet.blendFactor});
  for (const auto &vertex : block.vertices) {
    result.push_back(vertex.offset);
    result.push_back(vertex.size);
    result.push_back(vertex.stride);
    result.push_back(vertex.bound);
  }
  for (uint32_t i = 0; i < block.stageCount; ++i) {
    const auto &stage = block.stages[i];
    result.push_back(static_cast<uint32_t>(stage.stage));
    result.push_back(stage.uniformReg);
    result.push_back(stage.streamout);
    for (const auto &range : stage.uniforms) {
      result.push_back(range.offset);
      result.push_back(range.size);
    }
    for (uint32_t j = 0; j < stage.textureCount; ++j) {
      const auto &binding = stage.textures[j];
      result.push_back(binding.view.slot);
      result.push_back(binding.view.generation);
      result.push_back(binding.swizzle);
      result.push_back(binding.reg);
      result.push_back(static_cast<uint32_t>(binding.nullDimension));
    }
    bytes(std::span{stage.samplers, stage.samplerCount});
    bytes(std::span{uniforms[i]});
  }
  bytes(std::span{runtime});
  return result;
}

int main() {
  setenv("CEMU_LATTE_PIPELINE", "3", 1);
  Context mode3;
  assert(mode3.Parallel() && mode3.Threaded() && mode3.Queued());
  setenv("CEMU_LATTE_PIPELINE", "2", 1);
  Context mode2;
  assert(!mode2.Parallel() && mode2.Threaded());
  setenv("CEMU_LATTE_PIPELINE", "0", 1);
  Context mode0;
  assert(!mode0.Parallel() && !mode0.Queued());

  ChunkWorkers workers;
  for (size_t draws : {size_t{0}, size_t{1}, size_t{3}, size_t{33}, size_t{97},
                       size_t{128}, size_t{513}, size_t{1025}, size_t{2048}})
    for (unsigned requested = 0; requested <= 4; ++requested) {
      std::vector<unsigned> visits(draws);
      const auto timing =
          workers.Run(draws, requested, [&](unsigned worker, ChunkRange range) {
            assert(range.begin < range.end);
            for (size_t i = range.begin; i < range.end; ++i)
              visits[i] = worker + 1;
          });
      assert(timing.count == ChunkWorkers::Count(draws, requested));
      for (auto visit : visits)
        assert(visit > 0 && visit <= timing.count);
    }
  // A throw cannot leave siblings running against destroyed job storage.
  std::atomic<unsigned> finished{0};
  bool threw = false;
  try {
    workers.Run(128, 4, [&](unsigned worker, ChunkRange) {
      if (worker == 1)
        throw std::runtime_error("Injected chunk failure");
      finished.fetch_add(1);
    });
  } catch (const std::runtime_error &) {
    threw = true;
  }
  assert(threw && finished == 3);

  // Capture the same resolved stream used by mode 2. Immediately overwrite
  // producer objects and retire/wrap the original arena after every capture.
  Arena arena(4096);
  std::vector<CapturedDraw> captured;
  std::vector<std::vector<uint64_t>> reference;
  for (uint32_t i = 0; i < 257; ++i) {
    DrawPacket packet{};
    DrawBlock block{};
    packet.pipeline = {i % 13, 1};
    packet.framebuffer = {i % 7, 3};
    packet.indices = {i, i + 1};
    packet.baseVertex = i * 3;
    packet.baseInstance = i + 9;
    packet.instanceCount = 2;
    packet.count = 51 + i;
    packet.indexCount = 128 + i;
    packet.indexType = i % 3;
    packet.stencilRef = i;
    packet.viewport = {float(i), 0, 1280, 720, 0, 1};
    packet.scissor = {0, int32_t(i), 1280, 720};
    packet.blendFactor[0] = float(i) / 257;
    block.vertices[i % 16] = {i * 256, 4096, 16, true};
    block.stageCount = 3;
    std::array<std::vector<std::byte>, 3> uniforms;
    for (uint32_t j = 0; j < 3; ++j) {
      auto &stage = block.stages[j];
      stage.stage = static_cast<D3D12Const::Stage>(j);
      stage.uniformReg = (i % 5 == 0) ? -1 : 1;
      stage.uniforms[0] = {i * 256, 256};
      stage.textureCount = 2;
      stage.textures[0] = {{i % 11, 2}, i, 0, Latte::E_DIM::DIM_2D};
      stage.textures[1] = {{}, i, -3, Latte::E_DIM::DIM_2D};
      stage.samplerCount = 2;
      stage.samplers[0].words[0] = i + j;
      if (stage.uniformReg >= 0) {
        uniforms[j].assign(64 + i % 31, static_cast<std::byte>(i + j));
        assert(arena.Copy(uniforms[j], packet.uniformBytes[j]));
      }
    }
    std::vector<std::byte> runtime(64, static_cast<std::byte>(i ^ 0x42));
    assert(arena.Copy(runtime, packet.runtime));
    reference.push_back(Emit(packet, block, uniforms, runtime));
    captured.emplace_back(packet, block, arena);
    arena.Retire(arena.End());
    arena.ResetEmpty();
    block = {};
    for (auto &bytes : uniforms)
      std::fill(bytes.begin(), bytes.end(), std::byte{0xff});
  }
  for (unsigned k = 1; k <= 4; ++k) {
    std::vector<std::vector<uint64_t>> recorded(captured.size());
    workers.Run(captured.size(), k, [&](unsigned, ChunkRange range) {
      for (size_t i = range.begin; i < range.end; ++i) {
        const auto &draw = captured[i];
        recorded[i] =
            Emit(draw.packet, draw.block, draw.uniforms, draw.runtime);
      }
    });
    assert(recorded == reference);
  }
  for (unsigned iteration = 0; iteration < 200; ++iteration) {
    const auto timing = workers.Run(4, 4, [](unsigned, ChunkRange) {}, false);
    assert(timing.criticalPathNs == 0);
    for (auto ns : timing.busyNs)
      assert(ns == 0);
  }
  std::cout << "Chunk scheduler and captured mode-2/mode-3 draw parity passed "
               "(K=1/2/3/4, arena reuse, failure, shutdown).\n";
}
