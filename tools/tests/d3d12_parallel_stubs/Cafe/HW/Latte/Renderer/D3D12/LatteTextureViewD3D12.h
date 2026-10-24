#pragma once
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
class LatteTextureViewD3D12 {
public:
  std::atomic<uint32> calls{0};
  D3D12_CPU_DESCRIPTOR_HANDLE GetRecordingSRV(uint32 swizzle) {
    calls.fetch_add(1);
    return {0x400 + swizzle * 32};
  }
};
