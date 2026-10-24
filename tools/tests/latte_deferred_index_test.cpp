#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteChunkWorkers.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include <iostream>

struct Result {
  uint32 maximum = 0, count = 0;
  Renderer::INDEX_TYPE type{};
  Renderer::IndexAllocation allocation{};
};
static Result Decode(const void *data, uint32 count, LatteIndexType type,
                     LattePrimitiveMode mode = LattePrimitiveMode::TRIANGLES) {
  Result result;
  LatteIndices_decode(data, type, count, mode, result.maximum, result.type,
                      result.count, result.allocation);
  return result;
}
template <class T> static void Check() {
  LattePipeline::ChunkWorkers workers;
  for (bool bigEndian : {false, true})
    for (uint32 count : {1, 3, 17, 67, 1024, 16384}) {
      LatteIndices_invalidateAll(g_renderer.get());
      std::vector<T> source(count), expected(count);
      for (uint32 i = 0; i < count; ++i) {
        expected[i] = T((i * 7 + 11) % 65535);
        if (!bigEndian)
          source[i] = expected[i];
        else if constexpr (sizeof(T) == 2)
          source[i] = _swapEndianU16(expected[i]);
        else
          source[i] = _swapEndianU32(expected[i]);
      }
      const auto type =
          sizeof(T) == 2
              ? (bigEndian ? LatteIndexType::U16_BE : LatteIndexType::U16_LE)
              : (bigEndian ? LatteIndexType::U32_BE : LatteIndexType::U32_LE);
      g_renderer->deferIndexConversion = false;
      const auto serial = Decode(source.data(), count, type);
      const std::vector<uint8> reference(
          static_cast<uint8 *>(serial.allocation.mem),
          static_cast<uint8 *>(serial.allocation.mem) + count * sizeof(T));
      LatteIndices_invalidateAll(g_renderer.get());
      g_renderer->deferIndexConversion = true;
      const auto result = Decode(source.data(), count, type);
      assert(result.maximum == serial.maximum && result.type == serial.type &&
             result.count == serial.count);
      assert(!result.allocation.mem);
      auto *allocation = static_cast<Renderer::Allocation *>(
          result.allocation.rendererInternal);
      assert(allocation->deferred);
      const auto hit = Decode(source.data(), count, type);
      assert(hit.allocation.rendererInternal ==
             result.allocation.rendererInternal);
      // Mutation after capture cannot affect the output seen by any worker.
      std::fill(source.begin(), source.end(), T{0});
      workers.Run(4, 4, [&](unsigned, LattePipeline::ChunkRange) {
        allocation->deferred->Finish();
      });
      assert(allocation->bytes == reference);
      const auto changed = Decode(source.data(), count, type);
      assert(changed.maximum == 0);
      assert(changed.allocation.rendererInternal !=
             result.allocation.rendererInternal);
    }
  LatteIndices_invalidateAll(g_renderer.get());
  std::array<T, 3> values{std::numeric_limits<T>::max(), T{3}, T{5}};
  const auto type =
      sizeof(T) == 2 ? LatteIndexType::U16_LE : LatteIndexType::U32_LE;
  LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.value =
      std::numeric_limits<T>::max();
  const auto restarted = Decode(values.data(), 3, type);
  assert(restarted.maximum == 5);
  LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.value = 0;
  const auto hit = Decode(values.data(), 3, type);
  assert(hit.maximum == std::numeric_limits<T>::max());
  assert(hit.allocation.rendererInternal ==
         restarted.allocation.rendererInternal);
  LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.value = UINT32_MAX;
}
int main() {
  g_renderer = std::make_unique<Renderer>();
  Check<uint16>();
  Check<uint32>();
  LatteIndices_invalidateAll(g_renderer.get());
  g_renderer->deferIndexConversion = true;
  const std::array<uint16, 4> quad{0, _swapEndianU16(1), _swapEndianU16(2),
                                   _swapEndianU16(3)};
  const auto expanded =
      Decode(quad.data(), 4, LatteIndexType::U16_BE, LattePrimitiveMode::QUADS);
  assert(expanded.allocation.mem && expanded.count == 6);
  LatteIndices_invalidateAll(g_renderer.get());
  assert(g_renderer->live == 0);
  std::cout << "Deferred index parity passed (U16/U32 BE/LE, exact cache "
               "mutation, restart changes, captured bytes, shared first use, "
               "expansion fallback).\n";
}
