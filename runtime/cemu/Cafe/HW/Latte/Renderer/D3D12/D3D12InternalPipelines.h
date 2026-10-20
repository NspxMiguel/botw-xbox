// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Ported from PrismFox67/Cemu-UWP; see runtime/cemu/VENDORED.md.

#pragma once

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/RendererShaderD3D12.h"

// shaders used by the renderer itself (surface copies). See D3D12SurfaceCopy.cpp
struct D3D12Renderer::InternalPipelines
{
	std::unique_ptr<RendererShaderD3D12> copyVS;
	std::unique_ptr<RendererShaderD3D12> copyPSDepthToColor;
	std::unique_ptr<RendererShaderD3D12> copyPSColorToDepth;
};
