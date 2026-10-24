#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ParallelRecorder.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureViewD3D12.h"
#include <iostream>

static std::vector<D3D12ResolvedDraw> Capture(size_t count) {
  static ID3D12PipelineState pipeline{42};
  static LatteTextureViewD3D12 texture;
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
      stage.stage = static_cast<D3D12Const::Stage>((j + 1) % 3);
      stage.uniformReg = 0;
      stage.textureCount = D3D12Const::kMaxSRVsPerStage;
      for (uint32 reg = 0; reg < stage.textureCount; ++reg) {
        auto &binding = stage.textures[reg];
        binding.reg = stage.textureCount - reg - 1;
        binding.view = reg % 5 ? LattePipeline::ResourceId{1, 1}
                               : LattePipeline::ResourceId{};
        binding.swizzle = (i + j + reg) % 17;
        if (reg % 7 == 0)
          binding.reg = -2 - binding.reg; // dimensional mismatch selects null
      }
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
      LattePipeline::ResolveTextureDescriptors(
          block.stages[j], draw.stages[j],
          [&](LattePipeline::ResourceId, uint32 swizzle) {
            return texture.GetSRV(swizzle);
          },
          [](Latte::E_DIM) { return D3D12_CPU_DESCRIPTOR_HANDLE{2}; });
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
        const auto stageIndex =
            static_cast<uint32>(expected.source.block.stages[j].stage);
        const auto address = recorded.cbvs[stageIndex * 4];
        assert(address != 0);
        assert(memcmp(reinterpret_cast<const void *>(address),
                      expected.source.uniforms[j].data(), 64) == 0);
        for (uint32 reg = 1; reg < 4; ++reg)
          assert(recorded.cbvs[stageIndex * 4 + reg] == 0x8000);
        const auto table = recorded.tables[stageIndex * 2] - 0x100000000;
        assert(device.descriptors.at(table) ==
               expected.stages[j].cbvAddress[4]);
        for (uint32 reg = 0; reg < D3D12Const::kMaxSRVsPerStage; ++reg) {
          // Mode 2 resolves GetSRV at the draw; compare descriptor contents,
          // not addresses (a reused CPU handle is not a texture identity).
          const auto &stage = expected.source.block.stages[j];
          // Independent mode-2 rule: reverse register mapping, absent views,
          // dimensional nulls, and the original captured swizzle.
          const auto &binding = stage.textures[stage.textureCount - reg - 1];
          const auto srv =
              binding.reg < -1 || !binding.view
                  ? D3D12_CPU_DESCRIPTOR_HANDLE{2}
                  : D3D12_CPU_DESCRIPTOR_HANDLE{0x400 + binding.swizzle * 32};
          assert(device.descriptors.at(
                     table + (D3D12Const::StageTableOffsetSRV(
                                  expected.source.block.stages[j].stage) +
                              reg) *
                                 32) == device.descriptors.at(srv.ptr));
        }
        const auto sampler = recorded.tables[stageIndex * 2 + 1] - 0x100000000;
        for (uint32 reg = 0; reg < expected.source.block.stages[j].samplerCount;
             ++reg)
          assert(device.descriptors.at(sampler + reg * 32) ==
                 expected.source.block.stages[j].samplers[reg].words[0]);
      }
      ++i;
    }
  }
  assert(i == draws.size());
  // Unique full-width sampler tables force rollover even with K=4.
  assert(rollovers > 0);
}
static void RewriteTextureSources(ID3D12Device &device, uint64 generation) {
  device.descriptors[2] = 0;
  for (uint32 swizzle = 0; swizzle < 17; ++swizzle)
    device.descriptors[0x400 + swizzle * 32] = generation * 1000 + swizzle;
}
int main() {
  ID3D12Device device;
  ID3D12Fence fence;
  ID3D12RootSignature root;
  D3D12ParallelRecorder recorder(&device, &fence, &root, {1}, {2}, {3}, 0x8000);
  auto draws = Capture(385);
  for (unsigned k = 1; k <= 4; ++k) {
    RewriteTextureSources(device, k);
    const auto timing = recorder.Record(draws, k, k, true, k);
    assert(timing.count == k);
    std::vector<ID3D12CommandList *> lists;
    recorder.AppendLists(lists);
    assert(lists.size() == k);
    Check(device, lists, draws);
    recorder.Submitted(k);
  }
  // No GPU completion yet: first list's upload/descriptor storage remains live.
  RewriteTextureSources(device, 5);
  const auto timing = recorder.Record(draws, 5, 4, false, 5);
  assert(timing.count == 4 && timing.criticalPathNs == 0);
  std::vector<ID3D12CommandList *> lists;
  recorder.AppendLists(lists);
  Check(device, lists, draws);
  recorder.Submitted(5);
  fence.completed = 5;
  for (unsigned submission = 6; submission < 12; ++submission) {
    RewriteTextureSources(device, submission);
    recorder.Record(draws, submission, 4, false, submission);
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
