// SPDX-License-Identifier: MIT
#pragma once
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
class D3D12Renderer;
class D3D12SwapChain;
class LatteTextureD3D12;
// All SDK types and entry points are private; the SDK-free build has no DLL dependency.
class D3D12Ffx
{
  public:
	explicit D3D12Ffx(D3D12Renderer &renderer);
	~D3D12Ffx();
	void Resize(uint32 iw, uint32 ih, uint32 ow, uint32 oh, D3D12SwapChain *chain);
	void Reset() noexcept; // submits recorded work, drains presents and waits for the GPU
	bool Upscale(ID3D12Resource *color, D3D12_RESOURCE_STATES &colorState,
				 ID3D12Resource *output, D3D12_RESOURCE_STATES &outputState,
				 LatteTextureD3D12 *depth);
	void PreparePresent(bool paused = false);
	void EndFrame();
	bool Generating() const;

  private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
