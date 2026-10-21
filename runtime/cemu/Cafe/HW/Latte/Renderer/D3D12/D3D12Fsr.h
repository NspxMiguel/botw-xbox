// SPDX-License-Identifier: MIT
#pragma once
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
class D3D12Renderer;
class D3D12SwapChain;
// Spatial FSR only. The source is the existing output shader at the internal resolution,
// preserving gamma, channel swizzle, orientation and graphic-pack output transforms.
class D3D12Fsr
{
  public:
	explicit D3D12Fsr(D3D12Renderer &renderer);
	~D3D12Fsr();
	void ResizeTargets(uint32 inputWidth, uint32 inputHeight, uint32 outputWidth, uint32 outputHeight);
	bool Matches(uint32 iw, uint32 ih, uint32 ow, uint32 oh) const
	{
		return iw == inWidth && ih == inHeight && ow == outWidth && oh == outHeight;
	}
	void BeginInput();
	D3D12_CPU_DESCRIPTOR_HANDLE InputRTV() const
	{
		return inputRTV;
	}
	void Draw(D3D12SwapChain &swapChain, sint32 x, sint32 y, uint32 width, uint32 height, bool clear);

  private:
	D3D12Renderer &renderer;
	ComPtr<ID3D12RootSignature> root;
	ComPtr<ID3D12PipelineState> easu, rcas;
	ComPtr<ID3D12Resource> input, scaled;
	D3D12_CPU_DESCRIPTOR_HANDLE inputRTV{}, scaledRTV{}, inputSRV{}, scaledSRV{};
	D3D12_RESOURCE_STATES inputState = D3D12_RESOURCE_STATE_COMMON;
	D3D12_RESOURCE_STATES scaledState = D3D12_RESOURCE_STATE_COMMON;
	uint32 inWidth = 0, inHeight = 0, outWidth = 0, outHeight = 0;
};
