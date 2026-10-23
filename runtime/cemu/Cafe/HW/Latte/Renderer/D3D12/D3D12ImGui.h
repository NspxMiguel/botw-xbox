// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Ported from PrismFox67/Cemu-UWP; see runtime/cemu/VENDORED.md.

#pragma once

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"

#include <memory>

class D3D12Renderer;
class RendererShaderD3D12;
struct ImDrawData;

// Minimal Dear ImGui renderer on top of the backend's own infrastructure (GLSL shaders compiled through the same
// translation path as game shaders, shared root signature and descriptor heaps). The stock imgui_impl_dx12 backend can't be used
// because it brings its own root signature and descriptor heap.
class D3D12ImGuiRenderer
{
  public:
	explicit D3D12ImGuiRenderer(D3D12Renderer *renderer);
	~D3D12ImGuiRenderer();

	void EnsureFontTexture();
	void DestroyFontTexture();

	void *CreateTexture(const uint8 *rgba, sint32 width, sint32 height);
	void DestroyTexture(void *texture);
	void UploadTexture(void *texture, const uint8 *rgba, uint32 width, uint32 firstRow, uint32 rows, bool last);

	void Render(ImDrawData *drawData, D3D12_CPU_DESCRIPTOR_HANDLE rtv, DXGI_FORMAT rtvFormat, uint32 targetWidth, uint32 targetHeight);

  private:
	struct Texture
	{
		ComPtr<ID3D12Resource> resource;
		D3D12_CPU_DESCRIPTOR_HANDLE srv{};
	};

	ID3D12PipelineState *GetPipeline(DXGI_FORMAT rtvFormat);

	D3D12Renderer *m_renderer;
	std::unique_ptr<RendererShaderD3D12> m_vs;
	std::unique_ptr<RendererShaderD3D12> m_ps;
	ComPtr<ID3D12PipelineState> m_pipeline;
	DXGI_FORMAT m_pipelineFormat = DXGI_FORMAT_UNKNOWN;
	void *m_fontTexture = nullptr;
};
