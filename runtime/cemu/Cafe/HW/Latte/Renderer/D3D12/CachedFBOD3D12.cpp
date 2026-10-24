// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Ported from PrismFox67/Cemu-UWP; see runtime/cemu/VENDORED.md.

// BotWRecomp: iOS also globs this directory; keep non-D3D12 builds platform-neutral.
#ifdef ENABLE_D3D12

#include "Cafe/HW/Latte/Renderer/D3D12/CachedFBOD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureViewD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"

CachedFBOD3D12::CachedFBOD3D12(D3D12Renderer *renderer, uint64 key)
	: LatteCachedFBO(key), m_renderer(renderer)
{
	recordingId = renderer->RegisterResource(this);
	for (uint32 i = 0; i < 8; ++i)
		m_colorViews[i] = static_cast<LatteTextureViewD3D12 *>(colorBuffer[i].texture);
	m_depthView = static_cast<LatteTextureViewD3D12 *>(depthBuffer.texture);
	m_renderer->InitializeFramebuffer(recordingId);
}

void CachedFBOD3D12::CreateNative()
{
	auto *renderer = m_renderer;
	uint64 h = 0xcbf29ce484222325ull;
	auto mix = [&h](uint64 v) {
		h ^= v;
		h *= 0x100000001B3ull;
	};
	for (uint32 i = 0; i < 8; i++)
	{
		auto *view = m_colorViews[i];
		if (!view)
		{
			m_rtvs[i] = renderer->GetNullRTV();
			m_rtvFormats[i] = DXGI_FORMAT_UNKNOWN;
			mix(0);
			continue;
		}
		if (view->GetRTVFormat() == DXGI_FORMAT_UNKNOWN)
		{
			cemuLog_logDebugOnce(LogType::Force, "D3D12: Texture format {:04x} cannot be used as a render target", (uint32)view->GetNativeDefinition().format);
			m_rtvs[i] = renderer->GetNullRTV();
			m_rtvFormats[i] = DXGI_FORMAT_UNKNOWN;
			mix(0);
			continue;
		}
		m_rtvs[i] = view->GetRTV();
		m_rtvFormats[i] = view->GetRTVFormat();
		m_isInteger[i] = view->GetBaseTexture()->GetFormatInfo().isInteger;
		m_numRTVs = i + 1;
		mix((uint64)m_rtvFormats[i] + 1);
	}
	auto *depthView = m_depthView;
	if (depthView)
	{
		m_hasDSV = true;
		m_dsv = depthView->GetDSV();
		m_dsvFormat = depthView->GetDSVFormat();
	}
	mix((uint64)m_dsvFormat + 0x1000);
	m_formatHash = h;
}

CachedFBOD3D12::~CachedFBOD3D12()
{
	m_renderer->ReleaseFramebuffer(recordingId);
	m_renderer->RetireResource(recordingId);
}

void CachedFBOD3D12::ReleaseNative()
{
	m_renderer->NotifyFBORelease(this);
}

LatteTextureViewD3D12 *CachedFBOD3D12::GetColorView(uint32 index) const
{
	return m_colorViews[index];
}

LatteTextureViewD3D12 *CachedFBOD3D12::GetDepthView() const
{
	return m_depthView;
}

#endif // ENABLE_D3D12
