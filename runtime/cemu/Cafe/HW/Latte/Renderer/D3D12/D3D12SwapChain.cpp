#if defined(BOTWRECOMP) && defined(_WIN32)
#include "../../../../../../src/host/uwp/first_frame.h"
#endif
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Ported from PrismFox67/Cemu-UWP; see runtime/cemu/VENDORED.md.

// BotWRecomp: iOS also globs this directory; keep non-D3D12 builds platform-neutral.
#ifdef ENABLE_D3D12

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12SwapChain.h"
#ifdef BOTW_PLATFORM_UWP
#include "../../../../../../src/host/uwp/host.h"
#include "../../../../../../src/host/uwp/output_policy.h"
#endif

#include <dxgi1_5.h>
#include <chrono>
#ifdef BOTWRECOMP
extern int g_target_fps;
#endif

D3D12SwapChain::D3D12SwapChain(ID3D12Device *device, IDXGIFactory4 *factory, ID3D12CommandQueue *queue, D3D12StagingDescriptorHeap *rtvHeap, TargetType type, void *nativeHandle, uint32 width, uint32 height)
	: m_device(device), m_rtvHeap(rtvHeap), m_type(type), m_width(std::max<uint32>(width, 1)), m_height(std::max<uint32>(height, 1)), m_factory(factory), m_queue(queue), m_nativeHandle(nativeHandle)
{
	if (type == TargetType::CoreWindow)
		m_nativeWindow = static_cast<IUnknown *>(nativeHandle);
	Recreate();
}

void D3D12SwapChain::ReleaseNative()
{
	ReleaseBufferViews();
	if (m_frameLatencyWaitable)
		CloseHandle(m_frameLatencyWaitable);
	m_frameLatencyWaitable = nullptr;
	m_swapChain.Reset();
	m_frameGeneration = m_hasDefinedContent = false;
}

bool D3D12SwapChain::Recreate() noexcept
try
{
	// Install only a fully usable replacement; preserve a presentable fallback.
	auto *factory = m_factory.Get();
	auto *queue = m_queue.Get();
	const auto type = m_type;
	void *nativeHandle = m_nativeHandle;
	// tearing (uncapped presentation with vsync off) is only available for HWND swap chains on desktop
	ComPtr<IDXGIFactory5> factory5;
	if (type == TargetType::HWND && SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory5))))
	{
		BOOL allowTearing = FALSE;
		if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
			m_allowTearing = allowTearing != FALSE;
	}

	DXGI_SWAP_CHAIN_DESC1 desc{};
	desc.Width = m_width;
	desc.Height = m_height;
	desc.Format = kFormat;
	desc.Stereo = FALSE;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = D3D12Const::kSwapChainBufferCount;
	desc.Scaling = (type == TargetType::HWND) ? DXGI_SCALING_NONE : DXGI_SCALING_STRETCH;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	desc.Flags = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
	if (type == TargetType::CoreWindow)
		desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

	ComPtr<IDXGISwapChain1> swapChain1;
	HRESULT hr = E_FAIL;
	switch (type)
	{
	case TargetType::HWND:
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
		hr = factory->CreateSwapChainForHwnd(queue, (HWND)nativeHandle, &desc, nullptr, nullptr, &swapChain1);
		if (SUCCEEDED(hr))
			factory->MakeWindowAssociation((HWND)nativeHandle, DXGI_MWA_NO_ALT_ENTER); // fullscreen is handled by the UI
#else
		cemuLog_log(LogType::Force, "D3D12: HWND swap chains are not available in UWP builds");
#endif
		break;
	case TargetType::CoreWindow:
		hr = factory->CreateSwapChainForCoreWindow(queue, (IUnknown *)nativeHandle, &desc, nullptr, &swapChain1);
		if (FAILED(hr))
		{
			// Pacing support is optional: keep native presentation/upscaling usable.
			desc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
			hr = factory->CreateSwapChainForCoreWindow(queue, (IUnknown *)nativeHandle, &desc, nullptr, &swapChain1);
		}
		break;
	case TargetType::Composition:
		hr = factory->CreateSwapChainForComposition(queue, &desc, nullptr, &swapChain1);
		break;
	}
	if (FAILED(hr))
	{
		cemuLog_log(LogType::Force, "D3D12: Failed to create swap chain: {}", D3D12_HResultToString(hr));
		return false;
	}
	return ReplaceNative(swapChain1.Get());
}
catch (...)
{
	cemuLog_log(LogType::Force, "D3D12: Swap chain recreation failed");
	return false;
}

D3D12SwapChain::~D3D12SwapChain()
{
	ReleaseBufferViews();
	if (m_frameLatencyWaitable)
		CloseHandle(m_frameLatencyWaitable);
	m_frameLatencyWaitable = nullptr;
	m_swapChain.Reset();
}

void D3D12SwapChain::CreateBufferViews()
{
	for (uint32 i = 0; i < D3D12Const::kSwapChainBufferCount; i++)
	{
		auto &b = m_buffers[i];
		D3D12_ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&b.resource)), "IDXGISwapChain::GetBuffer");
		D3D12_SetDebugName(b.resource.Get(), fmt::format("Backbuffer{}", i));
		b.rtv = m_rtvHeap->Allocate();
		D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
		rtvDesc.Format = kFormat;
		rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		m_device->CreateRenderTargetView(b.resource.Get(), &rtvDesc, b.rtv);
		D3D12_Checkpoint(m_device, "swap chain backbuffer RTV created");
		b.state = D3D12_RESOURCE_STATE_PRESENT;
	}
	m_currentIndex = m_swapChain->GetCurrentBackBufferIndex();
	m_hasDefinedContent = false;
}

void D3D12SwapChain::ReleaseBufferViews()
{
#ifdef BOTW_PLATFORM_UWP
	uwp::release_presentation_resources(m_swapChain.Get());
#endif
	for (auto &b : m_buffers)
	{
		if (b.rtv.ptr)
			m_rtvHeap->Free(b.rtv);
		b.rtv = {};
		b.resource.Reset();
	}
}

bool D3D12SwapChain::ReplaceNative(IDXGISwapChain1 *chain) noexcept
{
	ComPtr<IDXGISwapChain3> replacement;
	if (!chain || FAILED(chain->QueryInterface(IID_PPV_ARGS(&replacement))))
		return false;
	Buffer buffers[D3D12Const::kSwapChainBufferCount];
	try
	{
		for (uint32 i = 0; i < D3D12Const::kSwapChainBufferCount; ++i)
		{
			auto &buffer = buffers[i];
			if (FAILED(replacement->GetBuffer(i, IID_PPV_ARGS(&buffer.resource))))
				throw std::runtime_error("FFX backbuffer unavailable");
			buffer.rtv = m_rtvHeap->Allocate();
			D3D12_RENDER_TARGET_VIEW_DESC desc{};
			desc.Format = kFormat;
			desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			m_device->CreateRenderTargetView(buffer.resource.Get(), &desc, buffer.rtv);
		}
	}
	catch (...)
	{
		for (auto &buffer : buffers)
			if (buffer.rtv.ptr)
				m_rtvHeap->Free(buffer.rtv);
		return false;
	}
	ReleaseBufferViews();
	if (m_frameLatencyWaitable)
		CloseHandle(m_frameLatencyWaitable);
	m_frameLatencyWaitable = nullptr;
	m_swapChain = std::move(replacement);
	DXGI_SWAP_CHAIN_DESC1 actual{};
	if (SUCCEEDED(m_swapChain->GetDesc1(&actual)))
	{
		m_width = actual.Width;
		m_height = actual.Height;
#ifdef BOTW_PLATFORM_UWP
		if (m_type == TargetType::CoreWindow)
			uwp::game_output_extent.publish(m_width, m_height);
#endif
		cemuLog_log(LogType::Force, "D3D12: actual swap chain {}x{}, {} buffers, format {}", m_width, m_height, actual.BufferCount, unsigned(actual.Format));
	}
	// Recreate and SDK replacement share this guard. Never configure latency
	// on HWND/composition chains, or on the non-waitable CoreWindow fallback.
	DXGI_SWAP_CHAIN_DESC1 desc{};
	if (m_type == TargetType::CoreWindow && SUCCEEDED(m_swapChain->GetDesc1(&desc)) &&
		(desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
	{
		if (SUCCEEDED(m_swapChain->SetMaximumFrameLatency(1)))
			m_frameLatencyWaitable = m_swapChain->GetFrameLatencyWaitableObject();
	}
	if (m_type == TargetType::CoreWindow && !m_frameLatencyWaitable)
		cemuLog_log(LogType::Force, "D3D12: CoreWindow chain has no frame-latency pacing after replacement/recreation; manual FG unavailable");
	for (uint32 i = 0; i < D3D12Const::kSwapChainBufferCount; ++i)
		m_buffers[i] = std::move(buffers[i]);
	m_currentIndex = m_swapChain->GetCurrentBackBufferIndex();
	m_frameGeneration = m_hasDefinedContent = false;
	return true;
}

void D3D12SwapChain::Resize(uint32 width, uint32 height)
{
	width = std::max<uint32>(width, 1);
	height = std::max<uint32>(height, 1);
	if (!m_swapChain)
	{
		m_width = width;
		m_height = height;
		Recreate();
		return;
	}
	if (width == m_width && height == m_height)
		return;
	ReleaseBufferViews();
	DXGI_SWAP_CHAIN_DESC1 desc{};
	m_swapChain->GetDesc1(&desc);
	HRESULT hr = m_swapChain->ResizeBuffers(D3D12Const::kSwapChainBufferCount, width, height, kFormat, desc.Flags);
	if (FAILED(hr))
	{
		cemuLog_log(LogType::Force, "D3D12: ResizeBuffers failed: {}", D3D12_HResultToString(hr));
		// DXGI retains the old buffers after a rejected size. Restore their RTVs
		// so a rejected HDMI change cannot leave presentation without resources.
		CreateBufferViews();
		return;
	}
	m_width = width;
	m_height = height;
	CreateBufferViews();
#ifdef BOTW_PLATFORM_UWP
	if (m_type == TargetType::CoreWindow)
		uwp::game_output_extent.publish(m_width, m_height);
#endif
	cemuLog_log(LogType::Force, "D3D12: resized swap chain {}x{}", m_width, m_height);
}

bool D3D12SwapChain::Present(uint32 syncInterval, uint32 maxWaitMs)
{
	m_presentTimedOut = false;
	if (!m_swapChain)
		return false;
#ifdef BOTWRECOMP
	// The guest uses a 60 Hz vblank clock: one interval at 60 fps, two at 30.
	// Never use ALLOW_TEARING, including callers requesting an unsynced present.
	if (!(m_frameGeneration && m_type == TargetType::CoreWindow))
		syncInterval = m_frameGeneration ? 1 : (g_target_fps >= 60 ? 1 : 2);
#endif
	UINT flags = 0;
	if (syncInterval == 0 && m_allowTearing)
		flags |= DXGI_PRESENT_ALLOW_TEARING;
	const auto start = std::chrono::steady_clock::now();
	// One queued frame at most. The second present waits for the generated
	// frame's flip; SyncInterval holds it for exactly half a guest frame.
	if (m_frameGeneration && m_type == TargetType::CoreWindow)
	{
		if (!m_frameLatencyWaitable)
			return false;
		const DWORD wait = WaitForSingleObjectEx(m_frameLatencyWaitable, maxWaitMs, FALSE);
		m_presentTimedOut = wait == WAIT_TIMEOUT;
		if (wait != WAIT_OBJECT_0)
			return false;
	}
	HRESULT hr = m_swapChain->Present(syncInterval, flags);
#if defined(BOTWRECOMP) && defined(_WIN32)
	if (hr == S_OK)
		uwp::signal_first_frame();
#endif
	const double blockedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	m_presentStats.presents++;
	m_presentStats.blockedMs += blockedMs;
	m_presentStats.maxBlockedMs = std::max(m_presentStats.maxBlockedMs, blockedMs);
	m_currentIndex = m_swapChain->GetCurrentBackBufferIndex();
	m_hasDefinedContent = false;
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
		return false;
	if (FAILED(hr))
		cemuLog_logDebug(LogType::Force, "D3D12: Present returned {}", D3D12_HResultToString(hr));
	return !(m_frameGeneration && m_type == TargetType::CoreWindow && FAILED(hr));
}

D3D12SwapChain::PresentStats D3D12SwapChain::TakePresentStats()
{
	const PresentStats stats = m_presentStats;
	m_presentStats = {};
	return stats;
}

double D3D12SwapChain::MeasureRefreshRate()
{
	DXGI_FRAME_STATISTICS stats{};
	if (!m_swapChain || FAILED(m_swapChain->GetFrameStatistics(&stats)) || stats.SyncQPCTime.QuadPart == 0)
		return 0.0;
	double hz = 0.0;
	if (m_lastSyncQPCTime.QuadPart != 0 && stats.SyncQPCTime.QuadPart > m_lastSyncQPCTime.QuadPart && stats.SyncRefreshCount > m_lastSyncRefreshCount)
	{
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		const double seconds = double(stats.SyncQPCTime.QuadPart - m_lastSyncQPCTime.QuadPart) / double(frequency.QuadPart);
		if (seconds > 0.25)
			hz = double(stats.SyncRefreshCount - m_lastSyncRefreshCount) / seconds;
	}
	m_lastSyncRefreshCount = stats.SyncRefreshCount;
	m_lastSyncQPCTime = stats.SyncQPCTime;
	return hz;
}

void D3D12SwapChain::RequestResize(uint32 width, uint32 height)
{
	std::lock_guard _l(m_resizeMutex);
	m_resizePending = true;
	m_pendingWidth = width;
	m_pendingHeight = height;
}

bool D3D12SwapChain::ConsumeResizeRequest(uint32 &width, uint32 &height)
{
	std::lock_guard _l(m_resizeMutex);
	if (!m_resizePending)
		return false;
	m_resizePending = false;
	width = m_pendingWidth;
	height = m_pendingHeight;
	return true;
}

#endif // ENABLE_D3D12
