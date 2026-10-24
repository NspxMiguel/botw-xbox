#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ParallelRecorder.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureViewD3D12.h"
#include <iostream>

static std::vector<D3D12ResolvedDraw> Capture(size_t count) {
  static ID3D12PipelineState pipeline{42};
  LattePipeline::Arena arena(4096);
  std::vector<D3D12ResolvedDraw> draws;
  for (uint32 i = 0; i < count; ++i) {
    LattePipeline::DrawPacket packet{};
    LattePipeline::DrawBlock block{};
    packet.baseVertex = i + 1;
    packet.baseInstance = i + 7;
    packet.count = 17 + i;
    packet.indexCount = 25 + i;
    packet.indexType = i % 3;
    packet.instanceCount = 2;
    packet.stencilRef = i;
    packet.viewport = {float(i), 0, 1280, 720, 0, 1};
    packet.scissor = {0, int32_t(i), 1280, 720};
    packet.blendFactor[3] = float(i);
    block.stageCount = 3;
    for (uint32 j = 0; j < 3; ++j) {
      auto &stage = block.stages[j];
      stage.stage = static_cast<D3D12Const::Stage>(j);
      stage.uniformReg = 0;
      stage.samplerCount = 18;
      for (auto &sampler : stage.samplers)
        sampler.words[0] = 1 + (i > 0 && i % 17 == 0 ? i - 1 : i) * 3 + j;
      std::array<std::byte, 64> bytes;
      bytes.fill(
          static_cast<std::byte>((i > 0 && i % 17 == 0 ? i - 1 : i) + j));
      assert(arena.Copy(bytes, packet.uniformBytes[j]));
    }
    std::array<uint32, 16> runtime;
    runtime.fill(i);
    assert(arena.Copy(std::as_bytes(std::span{runtime}), packet.runtime));
    D3D12ResolvedDraw draw(packet, block, arena);
    draw.pipeline = &pipeline;
    draw.topology = 3;
    draw.blendConstants = draw.blendAlphaOnly = draw.stencil = true;
    draw.indices = {i * 256 + 0x1000, 4096, packet.indexType};
    draw.vertices[i % 16] = {i * 256 + 0x10000, 4096, 16};
    draw.targetCount = 2;
    draw.targets[0].ptr = 0x100 + i;
    draw.targets[1].ptr = 0x200 + i;
    draw.depth.ptr = 0x300 + i;
    draw.barriers.push_back({1, i, i + 1});
    for (uint32 j = 0; j < 3; ++j) {
      draw.stages[j].cbvAddress[4] = 0x10000 + i * 256 + j * 0x100000;
      draw.stages[j].cbvSize[4] = 256;
    }
    draws.push_back(std::move(draw));
    arena.Retire(arena.End());
    arena.ResetEmpty();
  }
  return draws;
}
static void Check(ID3D12Device &device,
                  const std::vector<ID3D12CommandList *> &lists,
                  const std::vector<D3D12ResolvedDraw> &draws) {
  size_t i = 0;
  unsigned rollovers = 0;
  for (auto *base : lists) {
    auto *list = static_cast<ID3D12GraphicsCommandList *>(base);
    assert(list->closed);
    rollovers += list->heapChanges - 1;
    for (const auto &recorded : list->draws) {
      const auto &expected = draws[i];
      const auto &packet = expected.source.packet;
      assert(recorded.pipeline == 42 && recorded.topology == 3);
      assert(recorded.vertices == expected.vertices);
      assert(recorded.count ==
             (packet.indexType ? packet.indexCount : packet.count));
      assert(recorded.indexed == (packet.indexType != 0));
      assert(recorded.instances == packet.instanceCount &&
             recorded.baseInstance == packet.baseInstance);
      assert(recorded.baseVertex == int32_t(packet.baseVertex));
      assert(recorded.stencil == packet.stencilRef);
      assert(recorded.targetCount == expected.targetCount &&
             recorded.depth == expected.depth.ptr);
      for (uint32 j = 0; j < expected.targetCount; ++j)
        assert(recorded.targets[j] == expected.targets[j].ptr);
      assert(memcmp(&recorded.viewport, &packet.viewport,
                    sizeof(packet.viewport)) == 0);
      assert(memcmp(&recorded.scissor, &packet.scissor,
                    sizeof(packet.scissor)) == 0);
      assert(recorded.barriers == expected.barriers);
      for (auto value : recorded.runtime)
        assert(value == i);
      for (auto value : recorded.blend)
        assert(value == float(i));
      for (uint32 j = 0; j < 3; ++j) {
        const auto address = recorded.cbvs[j * 4];
        assert(address != 0);
        assert(memcmp(reinterpret_cast<const void *>(address),
                      expected.source.uniforms[j].data(), 64) == 0);
        for (uint32 reg = 1; reg < 4; ++reg)
          assert(recorded.cbvs[j * 4 + reg] == 0x8000);
        const auto table = recorded.tables[j * 2] - 0x100000000;
        assert(device.descriptors.at(table) ==
               expected.stages[j].cbvAddress[4]);
        const auto sampler = recorded.tables[j * 2 + 1] - 0x100000000;
        assert(device.descriptors.at(sampler) ==
               expected.source.block.stages[j].samplers[0].words[0]);
      }
      ++i;
    }
  }
  assert(i == draws.size());
  // Unique full-width sampler tables force rollover even with K=4.
  assert(rollovers > 0);
}
int main() {
  ID3D12Device device;
  ID3D12Fence fence;
  ID3D12RootSignature root;
  D3D12ParallelRecorder recorder(&device, &fence, &root, {1}, {2}, {3}, 0x8000);
  auto draws = Capture(385);
  for (unsigned k = 1; k <= 4; ++k) {
    const auto timing = recorder.Record(draws, k, k, true);
    assert(timing.count == k);
    std::vector<ID3D12CommandList *> lists;
    recorder.AppendLists(lists);
    assert(lists.size() == k);
    Check(device, lists, draws);
    recorder.Submitted(k);
  }
  // No GPU completion yet: first list's upload/descriptor storage remains live.
  const auto timing = recorder.Record(draws, 5, 4, false);
  assert(timing.count == 4 && timing.criticalPathNs == 0);
  std::vector<ID3D12CommandList *> lists;
  recorder.AppendLists(lists);
  Check(device, lists, draws);
  recorder.Submitted(5);
  fence.completed = 5;
  for (unsigned submission = 6; submission < 12; ++submission) {
    recorder.Record(draws, submission, 4, false);
    lists.clear();
    recorder.AppendLists(lists);
    Check(device, lists, draws);
    recorder.Submitted(submission);
    fence.completed = submission;
  }
  std::cout << "Production native chunk recorder passed: complete per-draw "
               "state, ordered barriers/lists, sampler rollover, "
               "upload/descriptor fences, K=1/2/3/4.\n";
}
