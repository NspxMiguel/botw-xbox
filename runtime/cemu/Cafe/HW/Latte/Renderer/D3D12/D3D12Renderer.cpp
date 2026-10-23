// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Ported from PrismFox67/Cemu-UWP; see runtime/cemu/VENDORED.md.

// BotWRecomp: iOS also globs this directory; keep non-D3D12 builds platform-neutral.
#ifdef ENABLE_D3D12

#include <bit>

#include "Cafe/HW/Latte/Core/LatteProfiler.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "D3D12PackHooks.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12SwapChain.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12FrameInterpolator.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Fsr.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Ffx.h"
#include "../../../../../../src/log.h"
#if defined(BOTW_PLATFORM_UWP)
#include "../../../../../../src/host/uwp/quick_menu.h"
#endif
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Runtime.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12RootSignature.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelineCache.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ImGui.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ShaderCompiler.h"
#include "Cafe/HW/Latte/Renderer/D3D12/RendererShaderD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureViewD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/CachedFBOD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12TextureReadback.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12InternalPipelines.h"
#include "Cafe/HW/Latte/Core/LatteTextureReadbackInfo.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Runtime.h"
#include "config/ActiveSettings.h"
#include "WindowSystem.h"

#include <imgui.h>
#include "imgui/imgui_extension.h"

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12SimSeriesS.h"
#include "../../../../../../src/sim_series_s.h"

#if defined(BOTW_PLATFORM_UWP)
#include "../../../../../../src/host/uwp/host.h"
#endif
#include <dxgi1_4.h>
#include <chrono>
#include <thread>

static uint64 CurrentThreadCpuNs();
static thread_local bool s_recordComInitialized = false;

D3D12Renderer *D3D12Renderer::GetInstance()
{
	cemu_assert_debug(g_renderer && g_renderer->GetType() == RendererAPI::D3D12);
	return static_cast<D3D12Renderer *>(g_renderer.get());
}

std::vector<D3D12Renderer::AdapterInfo> D3D12Renderer::GetAdapters()
{
	std::vector<AdapterInfo> result;
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		return result;
	ComPtr<IDXGIAdapter1> adapter;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++)
	{
		DXGI_ADAPTER_DESC1 desc;
		adapter->GetDesc1(&desc);
		if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
		{
			std::wstring wname(desc.Description);
			AdapterInfo info;
			info.name = std::string(wname.begin(), wname.end());
			info.luid = ((uint64)(uint32)desc.AdapterLuid.HighPart << 32) | desc.AdapterLuid.LowPart;
			result.push_back(std::move(info));
		}
		adapter.Reset();
	}
	return result;
}

D3D12Renderer::D3D12Renderer()
	: Renderer(RendererAPI::D3D12)
{
	LatteRenderTarget_invalidateCachedState();
	CreateDevice();

	m_stagingViewHeap = std::make_unique<D3D12StagingDescriptorHeap>(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096, "StagingViews");
	m_stagingSamplerHeap = std::make_unique<D3D12StagingDescriptorHeap>(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1024, "StagingSamplers");
	m_stagingRTVHeap = std::make_unique<D3D12StagingDescriptorHeap>(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1024, "StagingRTVs");
	m_stagingDSVHeap = std::make_unique<D3D12StagingDescriptorHeap>(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 256, "StagingDSVs");
	m_gpuViewHeap = std::make_unique<D3D12GpuViewHeap>(m_device.Get(), this, D3D12Const::kGpuViewHeapSize);
	m_gpuSamplerHeap = std::make_unique<D3D12GpuSamplerHeap>(m_device.Get(), D3D12Const::kGpuSamplerHeapSize);
	m_samplerCache = std::make_unique<D3D12SamplerCache>(m_device.Get(), m_stagingSamplerHeap.get());

	m_uploadRing = std::make_unique<D3D12UploadRing>(m_device.Get(), this, kUploadRingSize, "UploadRing");
	m_indexAllocator = std::make_unique<D3D12UploadHeapAllocator>(m_device.Get(), this, 8 * 1024 * 1024, "IndexHeap");
	m_readbackRing = std::make_unique<D3D12ReadbackRing>(m_device.Get(), kReadbackRingSize, "ReadbackRing");

	CreateRootSignature();
	CreateNullDescriptors();

	m_pipelineCache = std::make_unique<D3D12PipelineCache>(this);

	std::string err;
	if (!D3D12ShaderCompiler::Initialize(m_highestShaderModel, err))
		throw std::runtime_error(err);
	RendererShaderD3D12::Init();

	if (sim_series_s::settings().enabled)
	{
		try
		{
			m_simSeriesS = std::make_unique<D3D12SimSeriesS>(*this, m_queue.Get());
		}
		catch (const std::exception &e)
		{
			// Diagnostic mode only: never keep the game from starting.
			cemuLog_log(LogType::Force, "Series S simulation unavailable: {}", e.what());
		}
	}

	// first command list
	ResetCommandList();
#ifdef BOTW_HAS_FFX
	m_ffx = std::make_unique<D3D12Ffx>(*this);
#endif
	D3D12_Checkpoint(m_device.Get(), "renderer constructed");
}

D3D12Renderer::~D3D12Renderer()
{
	LatteIndices_invalidateAll(this);
	m_operations.Finish();
	// Final native teardown runs with the worker parked, after its last GPU use.
	m_operations.Exclusive(m_pipeline, [&] {
		if (m_device)
			WaitForIdle();
		m_simSeriesS.reset();
		if (m_ffx)
			m_ffx->Reset();
		// Disabled SDK chains still use provider code; release them before unloading FFX.
		m_swapChainMain.reset();
		m_swapChainPad.reset();
		m_ffx.reset();
		m_fsr.reset();
		occlusionQuery_destroyAll();
		m_imgui.reset();
		m_pipelineCache.reset();
		m_internal.reset();
		m_deferredReleaser.ReleaseAll();
	});
	RendererShaderD3D12::Shutdown();
	RendererShaderD3D12::ShaderCacheLoading_Close();
	D3D12ShaderCompiler::Shutdown();
	m_operations.Stop(m_pipeline, *this);
	if (m_fenceEvent)
		CloseHandle(m_fenceEvent);
}

void D3D12Renderer::CreateDevice()
{
	UINT factoryFlags = 0;
#ifdef CEMU_DEBUG_ASSERT
	m_debugMode = true;
#endif
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
	char *envDebug = nullptr;
	size_t envLen = 0;
	if (_dupenv_s(&envDebug, &envLen, "CEMU_D3D12_DEBUG") == 0 && envDebug)
	{
		m_debugMode = envDebug[0] == '1';
		free(envDebug);
	}
#endif
	if (m_debugMode)
	{
		ComPtr<ID3D12Debug> debug;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
		{
			debug->EnableDebugLayer();
			factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
			cemuLog_log(LogType::Force, "D3D12: Debug layer enabled");
		}
	}
	D3D12_ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_dxgiFactory)), "CreateDXGIFactory2");

	// Device Removed Extended Data: the runtime records the GPU operations of every command list and the allocation of a
	// GPU page fault, which LogDeviceRemovedData() writes to the log. It is part of the runtime (no Graphics Tools needed)
	// and the only way to find out what killed the device on a machine without a debugger, like an Xbox
	m_dredEnabled = D3D12_EnableDeviceRemovedDiagnostics();
	cemuLog_log(LogType::Force, "D3D12: Device removed diagnostics (DRED) {}", m_dredEnabled ? "enabled" : "not available");

	// CEMU_D3D12_WARP=1 uses WARP, Microsoft's software reference implementation. Drivers differ in how lenient they are
	// (Intel rendered Mario Kart 8's button icons correctly, the Xbox didn't), WARP shows how the API is specified
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
	{
		char *envWarp = nullptr;
		size_t envWarpLen = 0;
		if (_dupenv_s(&envWarp, &envWarpLen, "CEMU_D3D12_WARP") == 0 && envWarp)
		{
			const bool useWarp = envWarp[0] == '1';
			free(envWarp);
			ComPtr<IDXGIAdapter> warp;
			if (useWarp && SUCCEEDED(m_dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device))))
				m_selectedDeviceName = "WARP (software)";
		}
	}
#endif

	// pick the adapter. 0 means "default"
	const uint64 requestedLuid = GetD3D12Settings().adapterLuid;
	ComPtr<IDXGIAdapter1> adapter;
	for (UINT i = 0; !m_device && m_dxgiFactory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++)
	{
		DXGI_ADAPTER_DESC1 desc;
		adapter->GetDesc1(&desc);
		if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
		{
			adapter.Reset();
			continue;
		}
		const uint64 luid = ((uint64)(uint32)desc.AdapterLuid.HighPart << 32) | desc.AdapterLuid.LowPart;
		if (requestedLuid != 0 && luid != requestedLuid)
		{
			adapter.Reset();
			continue;
		}
		if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device))))
		{
			m_adapter = adapter;
			std::wstring wname(desc.Description);
			m_selectedDeviceName = std::string(wname.begin(), wname.end());
			switch (desc.VendorId)
			{
			case 0x1002:
				m_vendor = GfxVendor::AMD;
				break;
			case 0x10DE:
				m_vendor = GfxVendor::Nvidia;
				break;
			case 0x8086:
				m_vendor = GfxVendor::Intel;
				break;
			default:
				m_vendor = GfxVendor::Generic;
				break;
			}
			break;
		}
		adapter.Reset();
	}
	if (!m_device)
	{
		// fall back to the default adapter (on Xbox this is the only one)
		D3D12_ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)), "D3D12CreateDevice");
		m_selectedDeviceName = "Default adapter";
	}
	cemuLog_log(LogType::Force, "D3D12: Using device {}", m_selectedDeviceName);

	// D3D12CreateDevice hands out the existing device of the adapter while anything still references it. After a GPU reset
	// that is a removed device if a reference to the old one survived (Xbox: BOTW stopped at high memory, the next launch
	// failed its first allocation). Nothing can be done with it in this process, so say so instead of a silent failure
	D3D12_ResetRemovalReport();
	if (const HRESULT removed = m_device->GetDeviceRemovedReason(); FAILED(removed))
	{
		cemuLog_log(LogType::Force, "D3D12: the new device is already removed ({}), a reference to the previous device is still alive", D3D12_HResultToString(removed));
		throw std::runtime_error("The graphics device was reset and can't be used again in this session. Restart Cemu to play again.");
	}

	if (m_debugMode)
	{
		ComPtr<ID3D12InfoQueue> infoQueue;
		if (SUCCEEDED(m_device.As(&infoQueue)))
		{
			infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
			infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
		}
	}

	// capabilities. Older runtimes reject shader models they don't know about, so query from the highest one down.
	// Shader model 5.1 (DXBC, the default FXC path) is supported by every D3D12 device. 6.0 is only needed for DXIL
	D3D12_FEATURE_DATA_SHADER_MODEL sm{};
	m_highestShaderModel = D3D_SHADER_MODEL_5_1;
	for (uint32 candidate = 0x67; candidate >= 0x60; candidate--)
	{
		sm.HighestShaderModel = (D3D_SHADER_MODEL)candidate;
		if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
		{
			m_highestShaderModel = sm.HighestShaderModel;
			break;
		}
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
	if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
		m_bindingTier = options.ResourceBindingTier;
	cemuLog_log(LogType::Force, "D3D12: shader model {:x}, resource binding tier {}", (uint32)m_highestShaderModel, (uint32)m_bindingTier);
	if (m_bindingTier < D3D12_RESOURCE_BINDING_TIER_3)
		cemuLog_log(LogType::Force, "D3D12: Resource binding tier {} detected. Tier 3 is recommended, shaders that use more than 14 uniform blocks per stage may fail", (uint32)m_bindingTier);
	D3D12Format::InitFormatSupport(m_device.Get(), m_formatSupport);

	// pipeline libraries (driver pipeline cache on disk). The Xbox UWP driver (SraKmd) removes the device with
	// DXGI_ERROR_DRIVER_INTERNAL_ERROR when one is created, instead of failing the call, so it is never used there
	D3D12_FEATURE_DATA_SHADER_CACHE shaderCache{};
	m_pipelineLibrarySupported = SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE, &shaderCache, sizeof(shaderCache))) &&
								 (shaderCache.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_LIBRARY) != 0;
	// single pipeline blobs (GetCachedBlob / CachedPSO): the same driver also removes the device on GetCachedBlob
	m_pipelineBlobsSupported = (shaderCache.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_SINGLE_PSO) != 0;
	if (m_selectedDeviceName.rfind("SraKmd", 0) == 0)
	{
		m_pipelineLibrarySupported = false;
		m_pipelineBlobsSupported = false;
	}
	cemuLog_log(LogType::Force, "D3D12: Pipeline cache: libraries {}, single pipeline blobs {} (shader cache flags 0x{:x})", m_pipelineLibrarySupported ? "supported" : "not supported",
				m_pipelineBlobsSupported ? "supported" : "not supported", (uint32)shaderCache.SupportFlags);
	TryEnableImplicitShaderCache();

	// queue + fence
	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	D3D12_ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue)), "CreateCommandQueue");
	D3D12_ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "CreateFence");
	m_fenceEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
	if (!m_fenceEvent)
		throw std::runtime_error("D3D12: CreateEventEx failed");

	// command list (created closed, reset in ResetCommandList)
	ComPtr<ID3D12CommandAllocator> allocator;
	D3D12_ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
	D3D12_ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&m_cmdList)), "CreateCommandList");
	m_cmdList->Close();
	m_freeAllocators.push_back({allocator, 0});
}

void D3D12Renderer::CreateRootSignature()
{
	ComPtr<ID3DBlob> blob;
	ComPtr<ID3DBlob> error;
	HRESULT hr = D3D12_SerializeCemuRootSignature(&blob, &error);
	if (FAILED(hr))
	{
		std::string msg = error ? std::string((const char *)error->GetBufferPointer(), error->GetBufferSize()) : D3D12_HResultToString(hr);
		throw std::runtime_error("D3D12: Failed to serialize root signature: " + msg);
	}
	D3D12_ThrowIfFailed(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)), "CreateRootSignature");
	D3D12_SetDebugName(m_rootSignature.Get(), "CemuRootSignature");
}

void D3D12Renderer::CreateNullDescriptors()
{
	m_nullRTV = m_stagingRTVHeap->Allocate();
	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
	rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	m_device->CreateRenderTargetView(nullptr, &rtvDesc, m_nullRTV);

	// Root descriptors cannot use the descriptor-heap null CBV. Keep a real zero
	// resource alive for the renderer lifetime, large enough for any legal cbuffer.
	D3D12_HEAP_PROPERTIES zeroHeap = D3D12_HeapProps(D3D12_HEAP_TYPE_UPLOAD);
	D3D12_RESOURCE_DESC zeroDesc = D3D12_BufferDesc(D3D12Const::kMaxConstantBufferBytes);
	D3D12_ThrowIfFailed(m_device->CreateCommittedResource(&zeroHeap, D3D12_HEAP_FLAG_NONE, &zeroDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_zeroConstantBuffer)), "CreateCommittedResource (zero constants)");
	void *zeroData = nullptr;
	D3D12_RANGE noRead{0, 0};
	D3D12_ThrowIfFailed(m_zeroConstantBuffer->Map(0, &noRead, &zeroData), "Map (zero constants)");
	memset(zeroData, 0, D3D12Const::kMaxConstantBufferBytes);
	m_zeroConstantBuffer->Unmap(0, nullptr);
	m_nullCBV = m_stagingViewHeap->Allocate();
	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
	cbvDesc.BufferLocation = 0;
	cbvDesc.SizeInBytes = 0;
	m_device->CreateConstantBufferView(&cbvDesc, m_nullCBV);

	m_nullUAV = m_stagingViewHeap->Allocate();
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
	m_device->CreateUnorderedAccessView(nullptr, nullptr, &uavDesc, m_nullUAV);

	auto makeNullSRV = [&](D3D12_SRV_DIMENSION dim) {
		D3D12_CPU_DESCRIPTOR_HANDLE h = m_stagingViewHeap->Allocate();
		D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.ViewDimension = dim;
		desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		switch (dim)
		{
		case D3D12_SRV_DIMENSION_TEXTURE1D:
			desc.Texture1D.MipLevels = 1;
			break;
		case D3D12_SRV_DIMENSION_TEXTURE2D:
			desc.Texture2D.MipLevels = 1;
			break;
		case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
			desc.Texture2DArray.MipLevels = 1;
			desc.Texture2DArray.ArraySize = 1;
			break;
		case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
			desc.TextureCubeArray.MipLevels = 1;
			desc.TextureCubeArray.NumCubes = 1;
			break;
		case D3D12_SRV_DIMENSION_TEXTURE3D:
			desc.Texture3D.MipLevels = 1;
			break;
		default:
			break;
		}
		m_device->CreateShaderResourceView(nullptr, &desc, h);
		return h;
	};
	m_nullSRV1D = makeNullSRV(D3D12_SRV_DIMENSION_TEXTURE1D);
	m_nullSRV2D = makeNullSRV(D3D12_SRV_DIMENSION_TEXTURE2D);
	m_nullSRV2DArray = makeNullSRV(D3D12_SRV_DIMENSION_TEXTURE2DARRAY);
	m_nullSRVCubeArray = makeNullSRV(D3D12_SRV_DIMENSION_TEXTURECUBEARRAY);
	m_nullSRV3D = makeNullSRV(D3D12_SRV_DIMENSION_TEXTURE3D);
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::GetNullSRV(Latte::E_DIM dim) const
{
	switch (dim)
	{
	case Latte::E_DIM::DIM_1D:
		return m_nullSRV1D;
	case Latte::E_DIM::DIM_2D_ARRAY:
		return m_nullSRV2DArray;
	case Latte::E_DIM::DIM_CUBEMAP:
		return m_nullSRVCubeArray;
	case Latte::E_DIM::DIM_3D:
		return m_nullSRV3D;
	default:
		return m_nullSRV2D;
	}
}

/* --- submission --- */

uint64 D3D12Renderer::GetCompletedSubmissionId()
{
	m_pipeline.CheckBack();
	m_completedFenceValue = std::max(m_completedFenceValue, m_fence->GetCompletedValue());
	if (m_completedFenceValue == UINT64_MAX)
		HandleDeviceError(m_device->GetDeviceRemovedReason(), "Fence (device removed)");
	while (!m_submissionSequences.empty() && m_submissionSequences.front().first <= m_completedFenceValue)
	{
		m_operations.Completed(m_submissionSequences.front().second);
		m_submissionSequences.pop_front();
	}
	m_operations.SetCompletedFence(m_completedFenceValue);
	UpdatePipelineProfile();
	return m_completedFenceValue;
}

void D3D12Renderer::WaitForSubmission(uint64 submissionId)
{
	m_pipeline.CheckBack();
	cemu_assert_debug(submissionId <= m_lastSubmittedFenceValue);
	if (GetCompletedSubmissionId() >= submissionId)
		return;
	D3D12_ThrowIfFailed(m_fence->SetEventOnCompletion(submissionId, m_fenceEvent), "SetEventOnCompletion");
	const auto start = std::chrono::steady_clock::now();
	WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
	m_fenceWaitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	GetCompletedSubmissionId();
}

void D3D12Renderer::WaitForIdle()
{
	if (m_operations.Running() && !m_operations.Recording())
	{
		BackendPacket packet{};
		packet.kind = BackendPacket::Kind::ShutdownDrain;
		ExecuteBackend(LattePipeline::Opcode::Shutdown, packet);
		return;
	}
	m_pipeline.CheckBack();
	if (m_hasRecordedWork)
		SubmitCommandList(false);
	WaitForSubmission(m_lastSubmittedFenceValue);
	ProcessFinishedSubmissions();
}

void D3D12Renderer::ResetCommandList()
{
	m_pipeline.CheckBack();
	// reuse the oldest allocator if the GPU is done with it
	ProcessFinishedSubmissions();
	if (!m_inFlightAllocators.empty() && m_inFlightAllocators.front().fenceValue <= m_completedFenceValue)
	{
		m_freeAllocators.push_back(std::move(m_inFlightAllocators.front()));
		m_inFlightAllocators.pop_front();
	}
	if (m_freeAllocators.empty())
	{
		CommandAllocatorEntry e;
		D3D12_ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&e.allocator)), "CreateCommandAllocator");
		e.fenceValue = 0;
		m_freeAllocators.push_back(std::move(e));
	}
	m_currentAllocator = std::move(m_freeAllocators.front().allocator);
	m_freeAllocators.pop_front();
	D3D12_ThrowIfFailed(m_currentAllocator->Reset(), "ID3D12CommandAllocator::Reset");
	D3D12_ThrowIfFailed(m_cmdList->Reset(m_currentAllocator.Get(), nullptr), "ID3D12GraphicsCommandList::Reset");
	if (m_dredEnabled) // identifies the command list in the device removed diagnostics
		D3D12_SetDebugName(m_cmdList.Get(), fmt::format("Cemu submission #{}", m_lastSubmittedFenceValue + 1));
	m_drawsInCommandList = 0;
	m_hasRecordedWork = false;
	InvalidateDrawState();
	BindRootSignatureAndHeaps();
	occlusionQuery_notifyBeginCommandList();
	if (m_simSeriesS)
		m_simSeriesS->BeginSegment();
	D3D12_Checkpoint(m_device.Get(), "command list reset");
}

void D3D12Renderer::SubmitCommandList_record(bool waitIdle)
{
	m_pipeline.CheckBack();
	LatteProfiler::Scope profileScope(LatteProfiler::Slot::Submit);
	occlusionQuery_notifyEndCommandList();
	LatteProfiler::Add(LatteProfiler::Counter::Submissions);
	if (!m_hasRecordedWork && m_pendingBarriers.empty())
		LatteProfiler::Add(LatteProfiler::Counter::EmptySubmissions);
	FlushBarriers();
	if (m_simSeriesS)
		m_simSeriesS->EndSegment();
	HRESULT hr = m_cmdList->Close();
	if (FAILED(hr))
		HandleDeviceError(hr, "ID3D12GraphicsCommandList::Close");
	ID3D12CommandList *lists[] = {m_cmdList.Get()};
	D3D12_Checkpoint(m_device.Get(), "command list closed");
	m_queue->ExecuteCommandLists(1, lists);
	const uint64 submissionId = m_lastSubmittedFenceValue + 1;
	hr = m_queue->Signal(m_fence.Get(), submissionId);
	if (FAILED(hr))
		HandleDeviceError(hr, "ID3D12CommandQueue::Signal");
	m_uploadRing->OnSubmit(submissionId);
	m_gpuViewHeap->OnSubmit(submissionId);
	m_inFlightAllocators.push_back({std::move(m_currentAllocator), submissionId});
	m_lastSubmittedFenceValue = submissionId;
	const auto coveredSequence = m_operations.Progress().recorded;
	m_operations.Submitted(coveredSequence);
	m_submissionSequences.emplace_back(submissionId, coveredSequence);
	UpdatePipelineProfile();
	m_statSubmitsPerFrame++;
	m_submitSoon = false;

	// The first submissions are waited for and logged, so a GPU that rejects startup work (seen on Xbox) is caught at the
	// exact submission that caused it instead of somewhere later
	constexpr uint64 kLoggedStartupSubmissions = 40;
	if (submissionId <= kLoggedStartupSubmissions)
	{
		const uint32 draws = m_drawsInCommandList;
		WaitForSubmission(submissionId);
		const HRESULT removed = m_device->GetDeviceRemovedReason();
		cemuLog_log(LogType::Force, "D3D12: startup submission #{} ({} draws): {}", submissionId, draws, SUCCEEDED(removed) ? "ok" : D3D12_HResultToString(removed));
		if (FAILED(removed))
			HandleDeviceError(removed, fmt::format("Startup submission #{}", submissionId).c_str());
	}
	else if (waitIdle)
		WaitForSubmission(submissionId);
	ResetCommandList();
}

void D3D12Renderer::ProcessFinishedSubmissions()
{
	m_pipeline.CheckBack();
	const uint64 previous = m_completedFenceValue;
	const uint64 completed = GetCompletedSubmissionId();
	m_uploadRing->Retire(completed);
	m_gpuViewHeap->Retire(completed);
	m_indexAllocator->Retire(completed);
	m_deferredReleaser.Retire(completed);
	m_resources.Retire(m_operations.Progress().recorded, completed,
					   m_operations.Running() ? &m_operations.RetiredResources() : nullptr);
	if (!m_pipeline.Threaded() && !m_operations.Recording() && completed != previous)
	{
		m_frontCompleted = completed;
		LatteTextureReadback_UpdateFinishedTransfers(false);
	}
}

void D3D12Renderer::BindRootSignatureAndHeaps()
{
	LatteProfiler::Scope profileScope(LatteProfiler::Slot::RootSignature);
	ID3D12DescriptorHeap *heaps[] = {m_gpuViewHeap->GetHeap(), m_gpuSamplerHeap->GetHeap()};
	m_cmdList->SetDescriptorHeaps(2, heaps);
	m_cmdList->SetGraphicsRootSignature(m_rootSignature.Get());
	// Internal passes can change root arguments even when they use the same signature.
	for (auto &table : m_state.currentViewTables)
		table = {};
	for (auto &table : m_state.currentSamplerTables)
		table = {};
	memset(m_state.currentRootCBVs, 0, sizeof(m_state.currentRootCBVs));
	m_state.runtimeDataValid = false;
	m_state.rootSignatureBound = true;
	const D3D12StageTableDesc emptyBindings{};
	for (uint32 s = 0; s < (uint32)D3D12Const::Stage::Count; ++s)
		BindStageRootCBVs(emptyBindings, (D3D12Const::Stage)s);
}

void D3D12Renderer::InvalidateDrawState()
{
	m_state.renderTargetsDirty = true;
	m_state.vertexBufferDirtyMask = 0xFFFF;
	m_state.viewportDirty = true;
	m_state.scissorDirty = true;
	m_state.currentPSO = nullptr;
	m_state.currentTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
	m_state.currentIndexBuffer = {};
	m_state.currentBlendFactor[0] = -1.0f;
	m_state.currentStencilRef = 0xFFFFFFFF;
	m_state.rootSignatureBound = false;
}

void D3D12Renderer::HandleDeviceError(HRESULT hr, const char *what)
{
	HRESULT reason = m_device ? m_device->GetDeviceRemovedReason() : hr;
	std::string msg = fmt::format("D3D12: {} failed ({}). Device removed reason: {}", what, D3D12_HResultToString(hr), D3D12_HResultToString(reason));
	cemuLog_log(LogType::Force, "{}", msg);
	if (FAILED(reason))
		LogDeviceRemovedData();
	// with the debug layer (CEMU_D3D12_DEBUG=1) the call that made e.g. Close fail is in the info queue, which otherwise
	// only goes to an attached debugger
	ComPtr<ID3D12InfoQueue> infoQueue;
	if (m_device && SUCCEEDED(m_device.As(&infoQueue)))
	{
		const uint64 count = infoQueue->GetNumStoredMessages();
		for (uint64 i = count > 20 ? count - 20 : 0; i < count; i++)
		{
			SIZE_T size = 0;
			if (FAILED(infoQueue->GetMessage(i, nullptr, &size)) || size == 0)
				continue;
			std::vector<uint8> buffer(size);
			auto *message = (D3D12_MESSAGE *)buffer.data();
			if (SUCCEEDED(infoQueue->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
				cemuLog_log(LogType::Force, "D3D12 debug layer: {}", std::string_view(message->pDescription, message->DescriptionByteLength ? message->DescriptionByteLength - 1 : 0));
		}
	}
	throw std::runtime_error(msg);
}

namespace
{
const char *_BreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op)
{
	static const char *const s_names[] = {
		"SetMarker",
		"BeginEvent",
		"EndEvent",
		"DrawInstanced",
		"DrawIndexedInstanced",
		"ExecuteIndirect",
		"Dispatch",
		"CopyBufferRegion",
		"CopyTextureRegion",
		"CopyResource",
		"CopyTiles",
		"ResolveSubresource",
		"ClearRenderTargetView",
		"ClearUnorderedAccessView",
		"ClearDepthStencilView",
		"ResourceBarrier",
		"ExecuteBundle",
		"Present",
		"ResolveQueryData",
		"BeginSubmission",
		"EndSubmission",
		"DecodeFrame",
		"ProcessFrames",
		"AtomicCopyBufferUint",
		"AtomicCopyBufferUint64",
		"ResolveSubresourceRegion",
		"WriteBufferImmediate",
		"DecodeFrame1",
		"SetProtectedResourceSession",
		"DecodeFrame2",
		"ProcessFrames1",
		"BuildRaytracingAccelerationStructure",
		"EmitRaytracingAccelerationStructurePostbuildInfo",
		"CopyRaytracingAccelerationStructure",
		"DispatchRays",
		"InitializeMetaCommand",
		"ExecuteMetaCommand",
		"EstimateMotion",
		"ResolveMotionVectorHeap",
		"SetPipelineState1",
		"InitializeExtensionCommand",
		"ExecuteExtensionCommand",
		"DispatchMesh",
	};
	return (uint32)op < std::size(s_names) ? s_names[op] : "Unknown";
}

std::string _DredName(const char *nameA, const wchar_t *nameW)
{
	if (nameA)
		return nameA;
	if (nameW)
	{
		std::wstring w(nameW);
		return std::string(w.begin(), w.end());
	}
	return "(unnamed)";
}
} // namespace

void D3D12Renderer::LogDeviceRemovedData()
{
	if (m_dredLogged)
		return;
	m_dredLogged = true;
	ComPtr<ID3D12DeviceRemovedExtendedData> dred;
	if (!m_dredEnabled || !m_device || FAILED(m_device.As(&dred)))
	{
		cemuLog_log(LogType::Force, "D3D12: No device removed diagnostics (DRED) available");
		return;
	}
	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
	if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs)))
	{
		uint32 nodeCount = 0;
		for (const D3D12_AUTO_BREADCRUMB_NODE *node = breadcrumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext)
		{
			const uint32 completed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
			nodeCount++;
			if (completed >= node->BreadcrumbCount)
				continue; // finished on the GPU, not the cause
			cemuLog_log(LogType::Force, "DRED: {} on {}: {} of {} operations completed", _DredName(node->pCommandListDebugNameA, node->pCommandListDebugNameW),
						_DredName(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW), completed, node->BreadcrumbCount);
			const uint32 first = completed > 8 ? completed - 8 : 0;
			const uint32 last = std::min<uint32>(node->BreadcrumbCount, completed + 8);
			for (uint32 i = first; i < last; i++)
				cemuLog_log(LogType::Force, "DRED:   [{}] {}{}", i, _BreadcrumbOpName(node->pCommandHistory[i]), i == completed ? "   <- not completed, likely the faulting operation" : "");
		}
		cemuLog_log(LogType::Force, "DRED: {} command list(s) recorded", nodeCount);
	}
	D3D12_DRED_PAGE_FAULT_OUTPUT pageFault{};
	if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault)) && pageFault.PageFaultVA != 0)
	{
		cemuLog_log(LogType::Force, "DRED: GPU page fault at address 0x{:016x}", (uint64)pageFault.PageFaultVA);
		for (const D3D12_DRED_ALLOCATION_NODE *a = pageFault.pHeadExistingAllocationNode; a; a = a->pNext)
			cemuLog_log(LogType::Force, "DRED:   existing allocation at that address: {} (type {})", _DredName(a->ObjectNameA, a->ObjectNameW), (uint32)a->AllocationType);
		for (const D3D12_DRED_ALLOCATION_NODE *a = pageFault.pHeadRecentFreedAllocationNode; a; a = a->pNext)
			cemuLog_log(LogType::Force, "DRED:   recently freed allocation at that address: {} (type {})", _DredName(a->ObjectNameA, a->ObjectNameW), (uint32)a->AllocationType);
	}
	else
		cemuLog_log(LogType::Force, "DRED: no GPU page fault recorded");
}

void D3D12Renderer::ReleaseResourceDeferred(ComPtr<ID3D12Resource> resource)
{
	if (!resource)
		return;
	ComPtr<IUnknown> unk;
	resource.As(&unk);
	ReleaseObjectDeferred(std::move(unk));
}

void D3D12Renderer::ReleaseObjectDeferred(ComPtr<IUnknown> object)
{
	if (m_operations.Running() && !m_operations.Recording())
	{
		BackendPacket packet{};
		packet.kind = BackendPacket::Kind::NativeRelease;
		packet.resources[0] = RegisterResource(std::addressof(object));
		ExecuteBackend(LattePipeline::Opcode::Release, packet);
		RetireResource(packet.resources[0]);
		return;
	}
	m_pipeline.CheckBack();
	m_deferredReleaser.Release(std::move(object), GetRecordingSubmissionId());
}

/* --- barriers --- */

void D3D12Renderer::TransitionTextureSlow(LatteTextureD3D12 *texture, uint32 firstMip, uint32 mipCount, uint32 firstSlice, uint32 sliceCount, D3D12_RESOURCE_STATES newState)
{
	const LatteTextureD3D12::TransitionKey key{firstMip, mipCount, firstSlice, sliceCount, (uint32)newState};
	LatteProfiler::Add(LatteProfiler::Counter::TransitionMisses);
	const uint32 planeCount = texture->GetPlaneCount();
	// whole resource transition if possible
	if (firstMip == 0 && mipCount == texture->GetMipCount() && firstSlice == 0 && sliceCount == texture->GetArraySize())
	{
		D3D12_RESOURCE_STATES first = texture->GetSubresourceState(0);
		if (texture->AllSubresourcesInState(first))
		{
			if (first == newState)
			{
				texture->RememberTransition(key);
				return;
			}
			m_pendingBarriers.push_back(D3D12_TransitionBarrier(texture->GetResource(), first, newState));
			texture->SetAllSubresourceStates(newState);
			texture->RememberTransition(key);
			return;
		}
	}
	for (uint32 plane = 0; plane < planeCount; plane++)
	{
		for (uint32 slice = firstSlice; slice < firstSlice + sliceCount; slice++)
		{
			for (uint32 mip = firstMip; mip < firstMip + mipCount; mip++)
			{
				const uint32 sub = texture->GetSubresourceIndex(mip, slice, plane);
				const D3D12_RESOURCE_STATES current = texture->GetSubresourceState(sub);
				if (current == newState)
					continue;
				m_pendingBarriers.push_back(D3D12_TransitionBarrier(texture->GetResource(), current, newState, sub));
				texture->SetSubresourceState(sub, newState);
			}
		}
	}
	texture->RememberTransition(key);
}

void D3D12Renderer::TransitionTextureAll(LatteTextureD3D12 *texture, D3D12_RESOURCE_STATES newState)
{
	TransitionTexture(texture, 0, texture->GetMipCount(), 0, texture->GetArraySize(), newState);
}

void D3D12Renderer::UAVBarrier(ID3D12Resource *resource)
{
	m_pipeline.CheckBack();
	m_pendingBarriers.push_back(D3D12_UAVBarrier(resource));
}

void D3D12Renderer::FlushBarriers()
{
	m_pipeline.CheckBack();
	if (m_pendingBarriers.empty())
		return;
	m_cmdList->ResourceBarrier((UINT)m_pendingBarriers.size(), m_pendingBarriers.data());
	m_statBarriersPerFrame += (uint32)m_pendingBarriers.size();
	m_pendingBarriers.clear();
	m_hasRecordedWork = true;
}

/* --- object lifetime notifications --- */

void D3D12Renderer::NotifyTextureRelease(LatteTextureD3D12 *texture)
{
	if (m_sceneDepth == texture)
		m_sceneDepth = nullptr;
	m_sceneDepthUses.erase(texture);
	// drop pending barriers that reference the texture, the resource will be released once the GPU is done with it
	// (barriers that were already recorded keep the resource alive through the deferred releaser)
	m_pendingBarriers.erase(std::remove_if(m_pendingBarriers.begin(), m_pendingBarriers.end(), [texture](const D3D12_RESOURCE_BARRIER &b) {
								return b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Transition.pResource == texture->GetResource();
							}),
							m_pendingBarriers.end());
}

void D3D12Renderer::NotifyTextureViewRelease(LatteTextureViewD3D12 *view)
{
	if (m_internal)
		surfaceCopy_notifyViewRelease(view);
}

void D3D12Renderer::NotifyFBORelease(CachedFBOD3D12 *fbo)
{
	if (m_state.activeFBO == fbo)
	{
		m_state.activeFBO = nullptr;
		m_state.renderTargetsDirty = true;
	}
}

/* --- initialization --- */

void D3D12Renderer::Initialize()
{
	// UWP constructs the device/surface on the CoreWindow caller, then starts
	// Latte. Transfer the drained initial ownership before any front work.
	m_pipeline.BindFrontThread();
	Renderer::Initialize();
	D3D12_Checkpoint(m_device.Get(), "Renderer::Initialize");
	m_imgui = std::make_unique<D3D12ImGuiRenderer>(this);
	D3D12_Checkpoint(m_device.Get(), "ImGui renderer created (internal shaders)");
	surfaceCopy_init();
	D3D12_Checkpoint(m_device.Get(), "surfaceCopy_init");
	m_frontCpuStart = CurrentThreadCpuNs();
	m_operations.SetCompletionHandler([this](const LattePipeline::Completion &completion) { AcceptCompletion(completion); });
	m_operations.Start(m_pipeline, *this, [this] { WorkerIdle(); }, [] {
		// The native collector is local to the recording worker. It is transported
		// at report boundaries, never merged into front wall-time coverage.
		s_recordComInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
		SetThreadDescription(GetCurrentThread(), L"Latte D3D12 recorder");
		LatteProfiler::s_state = {};
		LatteProfiler::s_state.enabled = LatteProfiler::IsConfigured();
		LatteProfiler::s_state.last = LatteProfiler::Clock::now(); }, [] { if (s_recordComInitialized) CoUninitialize(); });
}

void D3D12Renderer::Shutdown()
{
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::ShutdownDrain;
	ExecuteBackend(LattePipeline::Opcode::Shutdown, packet);
	DeleteFontTextures();
	m_pipelineCache->Clear();
	m_operations.Exclusive(m_pipeline, [&] {
		WaitForIdle();
		m_imgui.reset();
		surfaceCopy_shutdown();
		m_deferredReleaser.ReleaseAll();
	});
	Renderer::Shutdown();
	// a reset during teardown otherwise only shows up later, without breadcrumbs, when the launcher uses the device
	if (!D3D12_Checkpoint(m_device.Get(), "renderer shutdown"))
		LogDeviceRemovedData();
}

void D3D12Renderer::EnableDebugMode()
{
	// the debug layer has to be enabled before device creation, see CEMU_D3D12_DEBUG
}

bool D3D12Renderer::GetVRAMInfo(int &usageInMB, int &totalInMB) const
{
	ComPtr<IDXGIAdapter3> adapter3;
	if (m_adapter && SUCCEEDED(m_adapter.As(&adapter3)))
	{
		DXGI_QUERY_VIDEO_MEMORY_INFO info{};
		if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
		{
			totalInMB = (int)(info.Budget / 1000 / 1000);
			usageInMB = (int)(info.CurrentUsage / 1000 / 1000);
			return true;
		}
	}
	return Renderer::GetVRAMInfo(usageInMB, totalInMB);
}

/* --- surfaces --- */

void D3D12Renderer::InitializeSurface(const Vector2i &size, bool mainWindow, SurfaceType surfaceType, void *nativeHandle)
{
	if (m_operations.Running() && !m_operations.Recording())
	{
		m_operations.Exclusive(m_pipeline, [&] { InitializeSurface(size, mainWindow, surfaceType, nativeHandle); });
		return;
	}
	const auto type = static_cast<D3D12SwapChain::TargetType>(surfaceType);
	auto swapChain = std::make_unique<D3D12SwapChain>(m_device.Get(), m_dxgiFactory.Get(), m_queue.Get(), m_stagingRTVHeap.get(), type, nativeHandle, (uint32)size.x, (uint32)size.y);
	if (!swapChain->IsValid())
		throw std::runtime_error("D3D12: Failed to create swap chain");
	D3D12_Checkpoint(m_device.Get(), mainWindow ? "TV swap chain created" : "GamePad swap chain created");
	if (mainWindow)
	{
		if (m_ffx)
			m_ffx->Reset();
		if (m_swapChainMain)
			WaitForIdle();
		m_swapChainMain = std::move(swapChain);
		m_fsrResizePending = m_fsr != nullptr;
	}
	else
	{
		m_swapChainPad = std::move(swapChain);
		m_padActive.store(true, std::memory_order_release);
	}
}

void D3D12Renderer::ShutdownSurface(bool mainWindow)
{
	if (m_operations.Running() && !m_operations.Recording())
	{
		m_operations.Exclusive(m_pipeline, [&] { ShutdownSurface(mainWindow); });
		return;
	}
	if (mainWindow)
	{
		if (m_ffx)
			m_ffx->Reset();
		WaitForIdle();
		m_swapChainMain.reset();
		return;
	}
	if (m_operations.Recording())
	{
		WaitForIdle();
		m_swapChainPad.reset();
		m_padActive.store(false);
		return;
	}
	// called from the UI thread. Ask the render thread to release the pad swap chain and wait for it to do so
	m_padShutdownRequested = true;
	for (int i = 0; i < 100 && m_padShutdownRequested; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	if (m_padShutdownRequested)
	{
		// render thread is not running (e.g. no game loaded), destroy it here
		std::lock_guard _l(m_padSwapChainMutex);
		m_swapChainPad.reset();
		m_padShutdownRequested = false;
	}
}

void D3D12Renderer::ProcessPadSwapChainShutdown()
{
	if (!m_padShutdownRequested)
		return;
	std::lock_guard _l(m_padSwapChainMutex);
	WaitForIdle();
	m_swapChainPad.reset();
	m_padActive.store(false, std::memory_order_release);
	m_padShutdownRequested = false;
}

void D3D12Renderer::ResizeSurface(const Vector2i &size, bool mainWindow)
{
	m_surfaceResize[mainWindow ? 0 : 1].store((uint64(uint32(size.x)) << 32) | uint32(size.y), std::memory_order_release);
	m_operations.Wake();
}

IDXGISwapChain1 *D3D12Renderer::GetSwapChain(bool mainWindow)
{
	if (m_operations.Running() && !m_operations.Recording())
	{
		return m_operations.Exclusive(m_pipeline, [&] { return GetSwapChain(mainWindow); });
	}
	D3D12SwapChain *swapChain = GetSwapChainObj(mainWindow);
	return swapChain ? swapChain->GetSwapChain() : nullptr;
}

bool D3D12Renderer::IsPadWindowActive()
{
	if (m_operations.Running() && !m_operations.Recording())
		return m_padActive.load(std::memory_order_acquire);
	ProcessPadSwapChainShutdown();
	return m_swapChainPad != nullptr && m_swapChainPad->IsValid();
}

bool D3D12Renderer::AcquireBackbuffer(bool mainWindow)
{
	ServicePendingPresent(true);
	if (!mainWindow)
		ProcessPadSwapChainShutdown();
	D3D12SwapChain *swapChain = GetSwapChainObj(mainWindow);
	if (!swapChain || !swapChain->IsValid())
		return false;
	const auto resize = m_surfaceResize[mainWindow ? 0 : 1].exchange(0, std::memory_order_acq_rel);
	if (resize)
		swapChain->RequestResize(uint32(resize >> 32), uint32(resize));
	uint32 w, h;
	if (swapChain->ConsumeResizeRequest(w, h))
	{
		if (mainWindow && m_ffx)
			m_ffx->Reset();
		WaitForIdle();
		swapChain->Resize(w, h);
		if (mainWindow)
			m_fsrResizePending = true;
		D3D12_Checkpoint(m_device.Get(), "swap chain resized");
	}
	TransitionResource(swapChain->GetCurrentBackbuffer(), swapChain->GetCurrentBackbufferState(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	D3D12_Checkpoint(m_device.Get(), "backbuffer acquired");
	return true;
}

void D3D12Renderer::PresentSwapChain(bool mainWindow)
{
	LatteProfiler::Scope profileScope(LatteProfiler::Slot::Unaccounted);
	ServicePendingPresent(true);
	D3D12SwapChain *swapChain = GetSwapChainObj(mainWindow);
	if (!swapChain || !swapChain->IsValid())
		return;
	const int vsync = m_presentVsync;
	// fps.cpp owns game timing. Interpolated presents add an independent clock.
#ifdef BOTWRECOMP
	const bool interpolationWanted = false;
#else
	const bool interpolationWanted = mainWindow && vsync != 0 && m_presentInterpolation;
#endif
	if (mainWindow)
		UpdatePresentDiagnostics(swapChain, interpolationWanted);
	const bool interpolate = interpolationWanted && !m_interpolationPaused;
	if (interpolate && swapChain->HasDefinedContent() && PresentInterpolated(swapChain))
		return;
	if (m_frameInterpolator && mainWindow)
		m_frameInterpolator->Reset();
	if (!swapChain->HasDefinedContent())
	{
		// make sure the backbuffer content is defined
		AcquireBackbuffer(mainWindow);
		FlushBarriers();
		const float clearColor[4] = {0, 0, 0, 1};
		m_cmdList->ClearRenderTargetView(swapChain->GetCurrentRTV(), clearColor, 0, nullptr);
	}
	if (mainWindow && m_simSeriesS)
		m_simSeriesS->EndFrame();
	if (mainWindow && m_ffx)
	{
#if defined(BOTW_PLATFORM_UWP)
		m_ffx->PreparePresent(swapChain->GetTargetType() == D3D12SwapChain::TargetType::CoreWindow && uwp::quick_menu.requested.load());
#else
		m_ffx->PreparePresent();
#endif
		swapChain->SetFrameGeneration(m_ffx->Generating());
		if (m_ffx->PresentGenerated())
		{
			const HRESULT removed = m_device->GetDeviceRemovedReason();
			if (FAILED(removed))
				HandleDeviceError(removed, "Manual FG Present");
			return;
		}
	}
	TransitionResource(swapChain->GetCurrentBackbuffer(), swapChain->GetCurrentBackbufferState(), D3D12_RESOURCE_STATE_PRESENT);
	SubmitCommandList(false);
#if defined(BOTW_PLATFORM_UWP)
	if (mainWindow && !uwp::quick_menu.requested.load())
		uwp::present_overlays(m_device.Get(), m_queue.Get(), swapChain->GetSwapChain()); // debug HUD, recorder
	if (mainWindow && uwp::quick_menu.requested.load())
	{
		// The quick menu presents synchronously while guest execution is paused.
		if (m_ffx)
			m_ffx->PreparePresent(true);
		swapChain->SetFrameGeneration(false);
		uwp::present_quick_menu(m_device.Get(), m_queue.Get(), swapChain->GetSwapChain());
		// The menu rotated the wrapped backbuffers; refresh the renderer's index.
		swapChain->RefreshCurrentIndex();
		const HRESULT removed = m_device->GetDeviceRemovedReason();
		if (FAILED(removed))
			HandleDeviceError(removed, "Present");
	}
#endif
	if (!swapChain->Present(vsync != 0 ? 1 : 0))
		HandleDeviceError(m_device->GetDeviceRemovedReason(), "Present");
	D3D12_Checkpoint(m_device.Get(), "present");
}

void D3D12Renderer::ServicePendingPresent(bool force)
{
	if (!m_pendingPresentSwapChain)
		return;
	if (!force && std::chrono::steady_clock::now() < m_pendingPresentAt)
		return;
	LatteProfiler::Scope profileScope(LatteProfiler::Slot::Unaccounted);
	D3D12SwapChain *swapChain = m_pendingPresentSwapChain;
	m_pendingPresentSwapChain = nullptr;
	if (!swapChain->Present(0))
		HandleDeviceError(m_device->GetDeviceRemovedReason(), "Present");
	D3D12_Checkpoint(m_device.Get(), "present (pending real frame)");
}

void D3D12Renderer::UpdatePresentDiagnostics(D3D12SwapChain *swapChain, bool interpolationWanted)
{
	const auto now = std::chrono::steady_clock::now();
	if (m_presentWindowStart == std::chrono::steady_clock::time_point{})
	{
		m_presentWindowStart = now;
		m_presentLogStart = now;
		LatteProfiler::Reset(now);
		extern std::atomic<uint64> g_latteIdleNs;
		if (!m_pipeline.Threaded())
			g_latteIdleNs.exchange(0);
		m_backIdleObserved = m_pipeline.Threaded() ? g_latteIdleNs.load() : 0;
		m_fenceWaitMs = 0;
		m_latteCpuTime = CurrentThreadCpuNs() / 100;
		swapChain->TakePresentStats();
		swapChain->MeasureRefreshRate();
		return;
	}
	++m_presentLogFrames; // Main swap chain calls count source game frames, not interpolated presents.
	const double seconds = std::chrono::duration<double>(now - m_presentWindowStart).count();
	if (seconds < 2.0)
		return;
	m_presentWindowStart = now;
	const D3D12SwapChain::PresentStats stats = swapChain->TakePresentStats();
	const double hz = swapChain->MeasureRefreshRate();
	// Keep the 2 s refresh-rate checks, but log and profile the same complete 10 s interval.
	extern std::atomic<uint64> g_latteIdleNs;
	if (m_pipeline.Threaded())
	{
		const auto idle = g_latteIdleNs.load();
		m_presentLogIdleNs += idle - m_backIdleObserved;
		m_backIdleObserved = idle;
	}
	else
		m_presentLogIdleNs += g_latteIdleNs.exchange(0);
	m_presentLogPresents += stats.presents;
	m_presentLogBlockedMs += stats.blockedMs;
	m_presentLogMaxBlockedMs = std::max(m_presentLogMaxBlockedMs, stats.maxBlockedMs);
	m_presentLogFenceWaitMs += m_fenceWaitMs;
	const double logSeconds = std::chrono::duration<double>(now - m_presentLogStart).count();
	if (logSeconds >= 10.0)
	{
		// This is the recorder in mode 2, the Latte thread otherwise.
		FILETIME created, exited, kernel, user;
		double cpuMs = 0;
		if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
		{
			const uint64 total = (uint64(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) + (uint64(user.dwHighDateTime) << 32 | user.dwLowDateTime);
			cpuMs = m_latteCpuTime ? (total - m_latteCpuTime) / 1e4 : 0;
			m_latteCpuTime = total;
			if (m_pipeline.Threaded())
				LatteProfiler::s_state.backCpuNs = uint64(cpuMs * 1e6);
		}
		cemuLog_log(LogType::Force, "D3D12: {:.1f} presents/s, Present blocked {:.1f} ms/frame (max {:.1f}), GPU fence wait {:.1f} ms/frame, Latte idle {:.0f}%, recording CPU {:.0f}%, {} cores",
					m_presentLogPresents / logSeconds, m_presentLogPresents ? m_presentLogBlockedMs / m_presentLogPresents : 0.0, m_presentLogMaxBlockedMs,
					m_presentLogPresents ? m_presentLogFenceWaitMs / m_presentLogPresents : 0.0, m_presentLogIdleNs / (logSeconds * 1e7),
					cpuMs / (logSeconds * 10), std::thread::hardware_concurrency());
		if (m_pipeline.Threaded())
		{
			m_reportSeconds = logSeconds;
			m_reportFrames = m_presentLogFrames;
		}
		else
		{
			LatteProfiler::s_state.frontCpuNs = uint64(cpuMs * 1e6);
			LatteProfiler::s_state.frontBusyNs = LatteProfiler::s_state.frontCpuNs;
			LatteProfiler::Report(now, logSeconds, m_presentLogFrames);
		}
		m_presentLogStart = now;
		m_presentLogFrames = 0;
		m_presentLogPresents = 0;
		m_presentLogBlockedMs = 0;
		m_presentLogMaxBlockedMs = 0;
		m_presentLogFenceWaitMs = 0;
		m_presentLogIdleNs = 0;
	}
	m_fenceWaitMs = 0;
	if (!interpolationWanted)
	{
		m_interpolationPaused = false;
		return;
	}
	const double avgBlockedMs = stats.presents ? stats.blockedMs / stats.presents : 0.0;
	// with frame statistics the refresh rate decides. Without them, Present() blocking about a 60 Hz refresh each time
	// while interpolating means the same (only judged while interpolating: one present per game frame blocks that long
	// on a 120 Hz display too)
	bool slowDisplay = m_interpolationPaused;
	if (hz > 0.0)
		slowDisplay = hz < 100.0;
	else if (!m_interpolationPaused && stats.presents > 30)
		slowDisplay = avgBlockedMs > 12.0;
	static const bool s_debugView = getenv("CEMU_INTERPOLATION_DEBUG") != nullptr; // presents once per frame on purpose
	if (s_debugView)
		slowDisplay = false;
	// the output itself runs at 100+ Hz (Xbox: HDMI 120 Hz, but vsync paces apps at 60 Hz): present both frames without
	// vsync and pace them here instead
	const double modeHz = g_hostDisplayModeRefreshHz.load();
	const bool unsynced = slowDisplay && modeHz >= 100.0;
	if (unsynced != m_interpolationUnsynced)
	{
		m_interpolationUnsynced = unsynced;
		cemuLog_log(LogType::Force, "D3D12: frame interpolation {} (vsync {:.2f} Hz, display mode {:.2f} Hz)",
					unsynced ? "presents without vsync, paced half a game frame apart" : "presents with vsync again", hz, modeHz);
	}
	if (unsynced)
		slowDisplay = false;
	if (slowDisplay != m_interpolationPaused)
	{
		m_interpolationPaused = slowDisplay;
		cemuLog_log(LogType::Force, "D3D12: frame interpolation {} (display {:.2f} Hz, Present blocks {:.1f} ms on average)",
					slowDisplay ? "paused, the display runs under 100 Hz and two presents per game frame would halve the game's speed" : "resumed", hz, avgBlockedMs);
	}
	if ((m_presentLogCounter++ % 15) == 0)
		cemuLog_log(LogType::Force, "D3D12: display {:.2f} Hz (DXGI frame statistics), {:.1f} presents/s, Present blocks {:.1f} ms average, {:.1f} ms max, frame interpolation {}",
					hz, stats.presents / seconds, avgBlockedMs, stats.maxBlockedMs, m_interpolationPaused ? "paused" : (m_interpolationUnsynced ? "active without vsync" : "active"));
}

// debugging aid: waits for the GPU and writes an RGBA8 texture as a 32 bit BMP
void D3D12Renderer::DumpTextureBMP(ID3D12Resource *texture, D3D12_RESOURCE_STATES &state, const std::string &path)
{
	const D3D12_RESOURCE_DESC desc = texture->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
	UINT numRows;
	UINT64 rowSize, totalBytes;
	m_device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &numRows, &rowSize, &totalBytes);
	D3D12_HEAP_PROPERTIES heap = D3D12_HeapProps(D3D12_HEAP_TYPE_READBACK);
	D3D12_RESOURCE_DESC bufferDesc{};
	bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufferDesc.Width = totalBytes;
	bufferDesc.Height = 1;
	bufferDesc.DepthOrArraySize = 1;
	bufferDesc.MipLevels = 1;
	bufferDesc.SampleDesc.Count = 1;
	bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ComPtr<ID3D12Resource> buffer;
	if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer))))
		return;
	TransitionResource(texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	FlushBarriers();
	D3D12_TEXTURE_COPY_LOCATION dst{buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
	dst.PlacedFootprint = layout;
	D3D12_TEXTURE_COPY_LOCATION src{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
	src.SubresourceIndex = 0;
	m_cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	SubmitCommandList(true);
	const uint8 *data = nullptr;
	if (FAILED(buffer->Map(0, nullptr, (void **)&data)))
		return;
	const uint32 width = (uint32)desc.Width, height = desc.Height;
	std::vector<uint8> file(54 + (size_t)width * height * 4);
	auto put32 = [&](size_t offset, uint32 v) { memcpy(file.data() + offset, &v, 4); };
	file[0] = 'B';
	file[1] = 'M';
	put32(2, (uint32)file.size());
	put32(10, 54);
	put32(14, 40);
	put32(18, width);
	put32(22, (uint32) - (sint32)height); // top-down
	file[26] = 1;
	file[28] = 32;
	for (uint32 y = 0; y < height; y++)
	{
		const uint8 *row = data + layout.Offset + (size_t)y * layout.Footprint.RowPitch;
		uint8 *out = file.data() + 54 + (size_t)y * width * 4;
		for (uint32 x = 0; x < width; x++)
		{
			out[x * 4 + 0] = row[x * 4 + 2];
			out[x * 4 + 1] = row[x * 4 + 1];
			out[x * 4 + 2] = row[x * 4 + 0];
			out[x * 4 + 3] = 255;
		}
	}
	buffer->Unmap(0, nullptr);
	std::error_code ec;
	fs::create_directories(fs::path(path).parent_path(), ec);
	FileStream *stream = FileStream::createFile2(_utf8ToPath(path));
	if (stream)
	{
		stream->writeData(file.data(), (sint32)file.size());
		delete stream;
	}
	cemuLog_log(LogType::Force, "D3D12: wrote {}", path);
}

// Frame interpolation: presents the frame halfway between the previous game frame and this one, then this one, each for
// one refresh. At 120 Hz the game keeps its 60 FPS while the screen shows 120. Returns false if interpolation isn't
// available, the caller then presents normally
bool D3D12Renderer::PresentInterpolated(D3D12SwapChain *swapChain)
{
	if (!m_frameInterpolator)
		m_frameInterpolator = std::make_unique<D3D12FrameInterpolator>(m_device.Get());
	D3D12FrameInterpolator &fi = *m_frameInterpolator;
	if (!fi.IsValid())
		return false;
	// game frame interval (smoothed), to pace the presents when they aren't vsynced
	const auto frameStart = std::chrono::steady_clock::now();
	if (m_lastGameFramePresent != std::chrono::steady_clock::time_point{})
	{
		const double ms = std::chrono::duration<double, std::milli>(frameStart - m_lastGameFramePresent).count();
		if (ms > 5.0 && ms < 50.0)
			m_gameFrameIntervalMs = m_gameFrameIntervalMs * 0.9 + ms * 0.1;
	}
	m_lastGameFramePresent = frameStart;
	const uint32 syncInterval = m_interpolationUnsynced ? 0 : 1;
	std::chrono::steady_clock::time_point interpolatedPresentedAt{};
	ID3D12Resource *backbuffer = swapChain->GetCurrentBackbuffer();
	const D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
	if (!fi.HasSize((uint32)desc.Width, desc.Height))
	{
		WaitForIdle(); // the old frames may still be in use by submitted work
		if (!fi.EnsureSize((uint32)desc.Width, desc.Height))
			return false;
	}
	auto transition = [this](ID3D12Resource *resource, D3D12_RESOURCE_STATES &state, D3D12_RESOURCE_STATES newState) { TransitionResource(resource, state, newState); };
	auto flushBarriers = [this]() { FlushBarriers(); };

	TransitionResource(backbuffer, swapChain->GetCurrentBackbufferState(), D3D12_RESOURCE_STATE_COPY_SOURCE);
	TransitionResource(fi.GetCurrentFrame(), fi.GetCurrentFrameState(), D3D12_RESOURCE_STATE_COPY_DEST);
	FlushBarriers();
	m_cmdList->CopyResource(fi.GetCurrentFrame(), backbuffer);
	if (fi.HasPreviousFrame())
	{
		// CEMU_INTERPOLATION_DUMP=1 writes three consecutive frames every 120 frames to interpolation_dump\ (a, b, c), plus the
		// frame interpolated between a and b. For tuning the interpolation offline: interpolating a and c can be compared to b
		static const bool s_dump = getenv("CEMU_INTERPOLATION_DUMP") != nullptr;
		static uint32 s_dumpCounter = 0;
		const uint32 dumpPhase = s_dumpCounter % 120, dumpIndex = s_dumpCounter / 120;
		s_dumpCounter++;
		if (s_dump && dumpPhase == 60)
		{
			DumpTextureBMP(fi.GetPreviousFrame(), fi.GetPreviousFrameState(), fmt::format("interpolation_dump/{:03}_a.bmp", dumpIndex));
			DumpTextureBMP(fi.GetCurrentFrame(), fi.GetCurrentFrameState(), fmt::format("interpolation_dump/{:03}_b.bmp", dumpIndex));
		}
		else if (s_dump && dumpPhase == 61)
			DumpTextureBMP(fi.GetCurrentFrame(), fi.GetCurrentFrameState(), fmt::format("interpolation_dump/{:03}_c.bmp", dumpIndex));
		fi.Interpolate(m_cmdList.Get(), transition, flushBarriers);
		if (s_dump && dumpPhase == 60)
			DumpTextureBMP(fi.GetOutput(), fi.GetOutputState(), fmt::format("interpolation_dump/{:03}_mid.bmp", dumpIndex));
		TransitionResource(fi.GetOutput(), fi.GetOutputState(), D3D12_RESOURCE_STATE_COPY_SOURCE);
		TransitionResource(backbuffer, swapChain->GetCurrentBackbufferState(), D3D12_RESOURCE_STATE_COPY_DEST);
		FlushBarriers();
		m_cmdList->CopyResource(backbuffer, fi.GetOutput());
		// CEMU_INTERPOLATION_DEBUG=1 shows only the interpolated frames, to judge their quality on a 60 Hz screen
		static const bool s_onlyInterpolated = getenv("CEMU_INTERPOLATION_DEBUG") != nullptr;
		TransitionResource(backbuffer, swapChain->GetCurrentBackbufferState(), D3D12_RESOURCE_STATE_PRESENT);
		if (s_onlyInterpolated)
		{
			SubmitCommandList(false);
			fi.Advance();
			if (!swapChain->Present(1))
				HandleDeviceError(m_device->GetDeviceRemovedReason(), "Present");
			return true;
		}
		SubmitCommandList(false);
		if (!swapChain->Present(syncInterval))
		{
			HandleDeviceError(m_device->GetDeviceRemovedReason(), "Present");
			return true;
		}
		interpolatedPresentedAt = std::chrono::steady_clock::now();
		// the real frame goes to the next backbuffer. Not through AcquireBackbuffer: a pending resize has to wait until
		// the next frame, the frame copy needs the current size
		ID3D12Resource *next = swapChain->GetCurrentBackbuffer();
		TransitionResource(fi.GetCurrentFrame(), fi.GetCurrentFrameState(), D3D12_RESOURCE_STATE_COPY_SOURCE);
		TransitionResource(next, swapChain->GetCurrentBackbufferState(), D3D12_RESOURCE_STATE_COPY_DEST);
		FlushBarriers();
		m_cmdList->CopyResource(next, fi.GetCurrentFrame());
		swapChain->SetDefinedContent();
	}
	TransitionResource(swapChain->GetCurrentBackbuffer(), swapChain->GetCurrentBackbufferState(), D3D12_RESOURCE_STATE_PRESENT);
	SubmitCommandList(false);
	fi.Advance();
	if (syncInterval == 0 && interpolatedPresentedAt != std::chrono::steady_clock::time_point{})
	{
		// without vsync nothing spaces the two presents: the real frame is due half a game frame after the halfway one.
		// Waiting here would stall the GPU thread, which goes on with the next frame, so its present is done from the
		// next draws (or the next use of the backbuffer) once it's due
		m_pendingPresentSwapChain = swapChain;
		m_pendingPresentAt = interpolatedPresentedAt + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(m_gameFrameIntervalMs * 0.5));
		D3D12_Checkpoint(m_device.Get(), "present (interpolated, real frame pending)");
		return true;
	}
	if (!swapChain->Present(syncInterval))
		HandleDeviceError(m_device->GetDeviceRemovedReason(), "Present");
	D3D12_Checkpoint(m_device.Get(), "present (interpolated)");
	return true;
}

void D3D12Renderer::ClearColorbuffer_record(bool padView)
{
	if (!AcquireBackbuffer(!padView))
		return;
	D3D12SwapChain *swapChain = GetSwapChainObj(!padView);
	FlushBarriers();
	const float clearColor[4] = {0, 0, 0, 0};
	m_cmdList->ClearRenderTargetView(swapChain->GetCurrentRTV(), clearColor, 0, nullptr);
	swapChain->SetDefinedContent();
	m_hasRecordedWork = true;
}

bool D3D12Renderer::BeginFrame(bool mainWindow)
{
	if (m_operations.Recording())
		return BeginFrame_record(mainWindow);
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::BeginFrame;
	packet.words[0] = mainWindow;
	return ExecuteBackend(LattePipeline::Opcode::Control, packet).words[0] != 0;
}

void D3D12Renderer::DrawEmptyFrame_record(bool mainWindow)
{
	if (!BeginFrame(mainWindow))
		return;
	SwapBuffers(mainWindow, !mainWindow);
}

// The Xbox's driver can't save pipelines for us (it removes the device on pipeline libraries and on GetCachedBlob), but
// its runtime has ID3D12Device9 (shader cache flags 0x60: control and session delete). Enabling the implicit caches lets
// the runtime keep what the driver compiles across launches, if the driver uses them. Unknown drivers may remove the
// device for this too, so the first try is guarded like the blob cache test: the marker says "testing" until frames were
// presented for a while, and a launch that finds "testing" knows the previous one died and leaves it off for good
void D3D12Renderer::TryEnableImplicitShaderCache()
{
	ComPtr<ID3D12Device9> device9;
	if (FAILED(m_device.As(&device9)))
	{
		cemuLog_log(LogType::Force, "D3D12: Implicit shader cache: no ID3D12Device9");
		return;
	}
	const fs::path marker = ActiveSettings::GetCachePath("shaderCache/driver/d3d12/implicit_cache_test.txt");
	m_implicitCacheMarker = marker;
	std::string state;
	{
		std::ifstream f(marker);
		std::getline(f, state);
	}
	auto writeMarker = [&marker](const char *text) {
		std::error_code ec;
		fs::create_directories(marker.parent_path(), ec);
		std::ofstream(marker, std::ios::trunc) << text << "\n";
	};
	if (state == "testing" || state == "failed")
	{
		if (state == "testing")
			writeMarker("failed");
		cemuLog_log(LogType::Force, "D3D12: Implicit shader cache: off, {}", state == "testing" ? "the previous launch ended while testing it" : "it failed before");
		return;
	}
	if (state != "ok")
	{
		writeMarker("testing");
		m_implicitCacheTesting = true;
	}
	// one kind at a time: the runtime rejects the whole call (E_INVALIDARG on a PC outside Windows developer mode) if any
	// kind can't be controlled
	const std::pair<D3D12_SHADER_CACHE_KIND_FLAGS, const char *> kinds[] = {
		{D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_D3D_CACHE_FOR_DRIVER, "D3D cache for driver"},
		{D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_DRIVER_MANAGED, "driver managed"},
		{D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_D3D_CONVERSIONS, "D3D conversions"},
	};
	std::string results;
	for (const auto &[kind, name] : kinds)
	{
		const HRESULT hr = device9->ShaderCacheControl(kind, D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE);
		results += fmt::format("{}{}: {}", results.empty() ? "" : ", ", name, D3D12_HResultToString(hr));
	}
	const HRESULT removed = m_device->GetDeviceRemovedReason();
	cemuLog_log(LogType::Force, "D3D12: Implicit shader cache: enable {}{}", results, FAILED(removed) ? fmt::format(", device removed: {}", D3D12_HResultToString(removed)) : std::string());
	if (FAILED(removed) && m_implicitCacheTesting)
	{
		writeMarker("failed");
		m_implicitCacheTesting = false;
	}
}

void D3D12Renderer::SwapBuffers_record(bool swapTV, bool swapDRC)
{
	if (m_implicitCacheTesting && swapTV && ++m_implicitCacheTestFrames >= 1200)
	{
		// 20 s of frames, with pipelines created on the loading screen and in game: the driver took it
		m_implicitCacheTesting = false;
		std::ofstream(m_implicitCacheMarker, std::ios::trunc) << "ok\n";
		cemuLog_log(LogType::Force, "D3D12: Implicit shader cache: test passed");
	}
	if (swapTV && m_swapChainMain)
		PresentSwapChain(true);
	if (swapDRC && m_swapChainPad)
		PresentSwapChain(false);
	if (swapTV && m_ffx)
		m_ffx->EndFrame();
	if (!swapTV && !swapDRC)
		SubmitCommandList(false);
	// All TV/DRC backbuffers have been submitted and presented. Apply the size
	// requested by the output pass for the next frame, never during acquisition.
	if (swapTV && m_fsrResizePending && !m_fsrFailed && m_fsrInputWidth && m_fsrInputHeight)
	{
		m_fsrResizePending = false;
		try
		{
			if (!m_fsr)
				m_fsr = std::make_unique<D3D12Fsr>(*this);
			m_fsr->ResizeTargets(m_fsrInputWidth, m_fsrInputHeight, m_fsrOutputWidth, m_fsrOutputHeight);
		}
		catch (const std::exception &e)
		{
			LOG("D3D12: FSR 1 unavailable; using output scaling: %s", e.what());
			m_fsrFailed = true;
		}
	}
	if (swapTV)
	{
		m_sceneDepth = nullptr;
		m_sceneDepthUses.clear();
	}

	m_statDrawsPerFrame = 0;
	m_statSubmitsPerFrame = 0;
	m_statBarriersPerFrame = 0;
}

void D3D12Renderer::Flush_record(bool waitIdle)
{
	if (m_hasRecordedWork || m_drawsInCommandList > 0)
		SubmitCommandList(false);
	if (waitIdle)
		WaitForIdle();
}

void D3D12Renderer::NotifyLatteCommandProcessorIdle_record()
{
	if (m_submitSoon && m_hasRecordedWork)
		SubmitCommandList(false);
}

void D3D12Renderer::AppendOverlayDebugInfo()
{
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::DebugSnapshot;
	const auto words = ExecuteBackend(LattePipeline::Opcode::Control, packet).words;
	ImGui::Text("--- D3D12 debug info ---");
	ImGui::Text("Device         %s", m_selectedDeviceName.c_str());
	ImGui::Text("ShaderModel    %x", (uint32)m_highestShaderModel);
	ImGui::Text("BindingTier    %u", (uint32)m_bindingTier);
	ImGui::Text("Pipelines      %u", m_pipelineCache->GetPipelineCount());
	ImGui::Text("Samplers       %u", (uint32)words[0]);
	ImGui::Text("SamplerHeap    %u / %u", (uint32)words[1], D3D12Const::kGpuSamplerHeapSize);
	ImGui::Text("UploadRing     %uKB / %uKB", (uint32)words[2], (uint32)words[7]);
	ImGui::Text("DeferredRel    %u", (uint32)words[3]);
	ImGui::Text("Draws/f        %u", (uint32)words[4]);
	ImGui::Text("Submits/f      %u", (uint32)words[5]);
	ImGui::Text("Barriers/f     %u", (uint32)words[6]);
	uint32 bufferCacheHeapSize = 0, bufferCacheAllocationSize = 0, bufferCacheNumAllocations = 0;
	LatteBufferCache_getStats(bufferCacheHeapSize, bufferCacheAllocationSize, bufferCacheNumAllocations);
	ImGui::Text("Buffer         %06uKB / %06uKB Allocs: %u", (bufferCacheAllocationSize + 1023) / 1024, (bufferCacheHeapSize + 1023) / 1024, bufferCacheNumAllocations);
}

/* --- imgui --- */

bool D3D12Renderer::ImguiBegin(bool mainWindow)
{
	if (!Renderer::ImguiBegin(mainWindow))
		return false;
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::UIAcquire;
	packet.words[0] = mainWindow;
	const auto reply = ExecuteBackend(LattePipeline::Opcode::Control, packet);
	if (!reply.words[0])
		return false;
	m_imgui->EnsureFontTexture();
	ImGui_UpdateWindowInformation(mainWindow);
	ImGui::NewFrame();
	m_imguiActive = true;
	m_imguiMainWindow = mainWindow;
	return true;
}

struct D3D12Renderer::UISnapshot
{
	ImDrawData data;
	std::vector<ImDrawList *> lists;
	explicit UISnapshot(const ImDrawData &source) : data(source)
	{
		lists.reserve(source.CmdListsCount);
		for (int i = 0; i < source.CmdListsCount; ++i)
			lists.push_back(source.CmdLists[i]->CloneOutput());
		data.CmdLists = lists.data();
	}
	~UISnapshot()
	{
		for (auto *list : lists)
			IM_DELETE(list);
	}
};
void D3D12Renderer::ImguiEnd()
{
	m_pipeline.CheckFront();
	ImGui::Render();
	UISnapshot snapshot(*ImGui::GetDrawData());
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::UIRender;
	packet.resources[0] = RegisterResource(&snapshot);
	packet.words[0] = m_imguiMainWindow;
	bool customCallback = false;
	for (const auto *list : snapshot.lists)
		for (const auto &command : list->CmdBuffer)
			customCallback |= command.UserCallback && command.UserCallback != ImDrawCallback_ResetRenderState;
	if (customCallback)
	{
		m_operations.Finish();
		m_operations.Exclusive(m_pipeline, [&] { m_pipeline.RunFrontCallback([&] { ui_record(packet); }); });
	}
	else
		ExecuteBackend(LattePipeline::Opcode::Control, packet);
	RetireResource(packet.resources[0]);
	m_imguiActive = false;
}

ImTextureID D3D12Renderer::GenerateTexture(const std::vector<uint8> &data, const Vector2i &size)
{
	// data is RGB8
	std::vector<uint8> rgba((size_t)size.x * size.y * 4);
	for (size_t i = 0; i < data.size() / 3 && i < (size_t)size.x * size.y; ++i)
	{
		rgba[i * 4 + 0] = data[i * 3 + 0];
		rgba[i * 4 + 1] = data[i * 3 + 1];
		rgba[i * 4 + 2] = data[i * 3 + 2];
		rgba[i * 4 + 3] = 0xFF;
	}
	return CreateUITexture(rgba.data(), size.x, size.y);
}

void D3D12Renderer::DeleteTexture(ImTextureID id)
{
	if (!m_imgui || !id)
		return;
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::UIRelease;
	packet.resources[0] = LattePipeline::FromOpaqueId(id);
	ExecuteBackend(LattePipeline::Opcode::Release, packet);
}

void D3D12Renderer::DeleteFontTextures()
{
	if (m_imgui)
		m_imgui->DestroyFontTexture();
}

/* --- shaders --- */

RendererShader *D3D12Renderer::shader_create(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, const std::string &source, bool isGameShader, bool isGfxPackShader)
{
	return new RendererShaderD3D12(type, baseHash, auxHash, isGameShader, isGfxPackShader, source);
}

RendererShaderD3D12 *D3D12Renderer::CreateInternalShader(RendererShader::ShaderType type, const std::string &glsl)
{
	auto *shader = new RendererShaderD3D12(type, 0, 0, false, false, glsl);
	shader->PreponeCompilation(true);
	D3D12_Checkpoint(m_device.Get(), "internal shader compiled");
	return shader;
}

void D3D12Renderer::SetPushConstants(const void *data, uint32 sizeInBytes)
{
	cemu_assert(sizeInBytes <= D3D12Const::kPushConstantDwords * 4);
	m_cmdList->SetGraphicsRoot32BitConstants(D3D12Const::kRootParamPushConstants, (sizeInBytes + 3) / 4, data, 0);
}

void D3D12Renderer::WriteCBV(D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_GPU_VIRTUAL_ADDRESS address, uint32 size)
{
	D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
	desc.BufferLocation = address;
	desc.SizeInBytes = AlignUp<uint32>(std::max<uint32>(size, 16), D3D12Const::kCBVAlignment);
	m_device->CreateConstantBufferView(&desc, dst);
}

bool D3D12Renderer::TryGetSamplerTable(const D3D12_SAMPLER_DESC *samplers, uint32 count, D3D12_GPU_DESCRIPTOR_HANDLE &tableOut)
{
	uint32 ids[D3D12Const::kMaxSamplersPerStage];
	D3D12_CPU_DESCRIPTOR_HANDLE handles[D3D12Const::kMaxSamplersPerStage];
	count = std::min<uint32>(count, D3D12Const::kMaxSamplersPerStage);
	if (count == 0)
	{
		// tables must point somewhere valid even if unused, use a single default sampler
		D3D12_SAMPLER_DESC def{};
		def.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		def.AddressU = def.AddressV = def.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		def.MaxLOD = D3D12_FLOAT32_MAX;
		def.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		handles[0] = m_samplerCache->GetSampler(def, ids[0]);
		count = 1;
	}
	else
	{
		for (uint32 i = 0; i < count; i++)
			handles[i] = m_samplerCache->GetSampler(samplers[i], ids[i]);
	}
	return m_gpuSamplerHeap->GetOrCreateTable(ids, handles, count, tableOut);
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::GetSamplerTable(const D3D12_SAMPLER_DESC *samplers, uint32 count)
{
	D3D12_GPU_DESCRIPTOR_HANDLE table{};
	if (TryGetSamplerTable(samplers, count, table))
		return table;
	// shader-visible sampler heap is full. Flush everything that references it and start over
	cemuLog_logDebug(LogType::Force, "D3D12: Sampler heap full, resetting");
	SubmitCommandList(true);
	m_gpuSamplerHeap->Reset();
	bool ok = TryGetSamplerTable(samplers, count, table);
	cemu_assert(ok);
	return table;
}

/* --- render targets --- */

void D3D12Renderer::renderTarget_setViewport(float x, float y, float width, float height, float nearZ, float farZ, bool halfZ)
{
	// halfZ is handled in the vertex shader (same as Vulkan). Unlike Vulkan we don't need a negative viewport height,
	// D3D's clip space Y already points up
	D3D12_VIEWPORT vp;
	vp.TopLeftX = x;
	vp.TopLeftY = y;
	vp.Width = width;
	vp.Height = height;
	vp.MinDepth = std::clamp(nearZ, 0.0f, 1.0f);
	vp.MaxDepth = std::clamp(farZ, 0.0f, 1.0f);
	if (memcmp(&vp, &m_frontState.viewport, sizeof(vp)) == 0)
		return;
	m_frontState.viewport = vp;
	if (!m_pipeline.Queued())
		renderTarget_setViewport_record(vp);
}

void D3D12Renderer::renderTarget_setScissor(sint32 scissorX, sint32 scissorY, sint32 scissorWidth, sint32 scissorHeight)
{
	D3D12_RECT r{scissorX, scissorY, scissorX + std::max(scissorWidth, 0), scissorY + std::max(scissorHeight, 0)};
	if (memcmp(&r, &m_frontState.scissor, sizeof(r)) == 0)
		return;
	m_frontState.scissor = r;
	if (!m_pipeline.Queued())
		renderTarget_setScissor_record(r);
}

LatteCachedFBO *D3D12Renderer::rendertarget_createCachedFBO(uint64 key)
{
	m_pipeline.BeginOperation();
	return new CachedFBOD3D12(this, key);
}

void D3D12Renderer::rendertarget_deleteCachedFBO(LatteCachedFBO *fbo)
{
	if (m_frontState.activeFBO == fbo)
		m_frontState.activeFBO = nullptr;
	if (m_pipeline.Queued())
		return;
	if (m_state.activeFBO == fbo)
	{
		m_state.activeFBO = nullptr;
		m_state.renderTargetsDirty = true;
	}
}

void D3D12Renderer::rendertarget_bindFramebufferObject(LatteCachedFBO *cfbo)
{
	m_pipeline.CheckFront();
	if (m_frontState.activeFBO == cfbo)
		return;
	m_frontState.activeFBO = static_cast<CachedFBOD3D12 *>(cfbo);
	if (m_pipeline.Queued())
		return;
	if (m_state.activeFBO != cfbo)
	{
		m_state.activeFBO = static_cast<CachedFBOD3D12 *>(cfbo);
		m_state.renderTargetsDirty = true;
	}
}

/* --- textures --- */

void *D3D12Renderer::texture_acquireTextureUploadBuffer(uint32 size)
{
	if (m_textureUploadBuffer.size() < size)
		m_textureUploadBuffer.resize(size);
	return m_textureUploadBuffer.data();
}

void D3D12Renderer::texture_releaseTextureUploadBuffer(uint8 *mem)
{
	cemu_assert_debug(mem == m_textureUploadBuffer.data());
}

TextureDecoder *D3D12Renderer::texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, uint32 width, uint32 height)
{
	D3D12TextureFormatInfo info;
	D3D12Format::GetTextureFormatInfo(m_formatSupport, format, isDepth, info);
	return info.decoder;
}

LatteTexture *D3D12Renderer::texture_createTextureEx(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels,
													 uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth)
{
	m_pipeline.BeginOperation();
	return new LatteTextureD3D12(this, dim, physAddress, physMipAddress, format, width, height, depth, pitch, mipLevels, swizzle, tileMode, isDepth);
}

void D3D12Renderer::texture_setLatteTexture(LatteTextureView *textureView, uint32 textureUnit)
{
	cemu_assert_debug(textureUnit < std::size(m_frontState.boundTexture));
	m_frontState.boundTexture[textureUnit] = static_cast<LatteTextureViewD3D12 *>(textureView);
}

void D3D12Renderer::texture_loadSlice(LatteTexture *hostTexture, sint32 width, sint32 height, sint32 depth, void *pixelData, sint32 sliceIndex, sint32 mipIndex, uint32 compressedImageSize)
{
	LatteProfiler::Scope profileScope(LatteProfiler::Slot::TextureLoad);
	auto *tex = static_cast<LatteTextureD3D12 *>(hostTexture);
	ID3D12Resource *resource = tex->GetResource();
	if (!resource)
		return;
	const D3D12TextureFormatInfo &fmt = tex->GetFormatInfo();
	const bool is3D = tex->Is3DTexture();
	if ((uint32)mipIndex >= tex->GetMipCount())
		return;

	const uint32 blockDim = fmt.isCompressed ? 4 : 1;
	const uint32 heightInBlocks = (uint32)(height + blockDim - 1) / blockDim;
	bool first = true;
	auto copyPlane = [&](uint32 plane, const uint8 *source, uint32 rowBytes) {
		texture_preparePlane(tex, mipIndex, sliceIndex, plane, source, rowBytes, heightInBlocks, width, blockDim, first);
		first = false;
	};
	if (!fmt.hasStencil)
	{
		const uint32 rowBytes = heightInBlocks ? compressedImageSize / heightInBlocks : 0;
		if (tex->overwriteInfo.hasFormatOverwrite && tex->format == Latte::E_GX2SURFFMT::R10_G10_B10_A2_UNORM &&
			tex->overwriteInfo.format == uint32(Latte::E_GX2SURFFMT::R16_G16_B16_A16_UNORM))
		{
			auto converted = d3d12_packs::expand_rgb10a2(pixelData, compressedImageSize / 4);
			copyPlane(0, reinterpret_cast<const uint8 *>(converted.data()), rowBytes * 2);
		}
		else
		{
			copyPlane(0, (const uint8 *)pixelData, rowBytes);
		}
	}
	else
	{
		// depth/stencil formats are planar in D3D12. Split the interleaved guest data
		const uint32 texelCount = (uint32)width * (uint32)height;
		const uint32 bytesPerTexel = texelCount ? compressedImageSize / texelCount : 0;
		std::vector<uint8> depthPlane((size_t)texelCount * 4);
		std::vector<uint8> stencilPlane(texelCount);
		const uint8 *src = (const uint8 *)pixelData;
		if (bytesPerTexel == 4)
		{
			// D24S8: depth in the low 24 bits, stencil in the high 8 bits. Plane 0 is a 32 bit footprint with depth in the low bits
			for (uint32 i = 0; i < texelCount; i++)
			{
				uint32 v;
				memcpy(&v, src + i * 4, 4);
				uint32 d = v & 0xFFFFFF;
				memcpy(depthPlane.data() + i * 4, &d, 4);
				stencilPlane[i] = (uint8)(v >> 24);
			}
		}
		else if (bytesPerTexel == 8)
		{
			// D32F + S8 + X24
			for (uint32 i = 0; i < texelCount; i++)
			{
				memcpy(depthPlane.data() + i * 4, src + i * 8, 4);
				stencilPlane[i] = src[i * 8 + 4];
			}
		}
		else
		{
			cemuLog_logDebugOnce(LogType::Force, "D3D12: Unexpected depth/stencil upload size");
			return;
		}
		copyPlane(0, depthPlane.data(), (uint32)width * 4);
		copyPlane(1, stencilPlane.data(), (uint32)width);
	}
}

static D3D12_CPU_DESCRIPTOR_HANDLE _CreateTempRTV(D3D12Renderer *r, LatteTextureD3D12 *tex, sint32 sliceIndex, sint32 mipIndex)
{
	D3D12_RENDER_TARGET_VIEW_DESC desc{};
	desc.Format = tex->GetFormatInfo().rtvFormat;
	if (tex->Is3DTexture())
	{
		desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
		desc.Texture3D.MipSlice = mipIndex;
		desc.Texture3D.FirstWSlice = sliceIndex;
		desc.Texture3D.WSize = 1;
	}
	else if (tex->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
	{
		desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
		desc.Texture1DArray.MipSlice = mipIndex;
		desc.Texture1DArray.FirstArraySlice = sliceIndex;
		desc.Texture1DArray.ArraySize = 1;
	}
	else
	{
		desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
		desc.Texture2DArray.MipSlice = mipIndex;
		desc.Texture2DArray.FirstArraySlice = sliceIndex;
		desc.Texture2DArray.ArraySize = 1;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE h = r->GetStagingRTVHeap().Allocate();
	r->GetDevice()->CreateRenderTargetView(tex->GetResource(), &desc, h);
	return h;
}

static D3D12_CPU_DESCRIPTOR_HANDLE _CreateTempDSV(D3D12Renderer *r, LatteTextureD3D12 *tex, sint32 sliceIndex, sint32 mipIndex)
{
	D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
	desc.Format = tex->GetFormatInfo().dsvFormat;
	if (tex->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
	{
		desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
		desc.Texture1DArray.MipSlice = mipIndex;
		desc.Texture1DArray.FirstArraySlice = sliceIndex;
		desc.Texture1DArray.ArraySize = 1;
	}
	else
	{
		desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		desc.Texture2DArray.MipSlice = mipIndex;
		desc.Texture2DArray.FirstArraySlice = sliceIndex;
		desc.Texture2DArray.ArraySize = 1;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE h = r->GetStagingDSVHeap().Allocate();
	r->GetDevice()->CreateDepthStencilView(tex->GetResource(), &desc, h);
	return h;
}

void D3D12Renderer::texture_clearSlice(LatteTexture *hostTexture, sint32 sliceIndex, sint32 mipIndex)
{
	auto *tex = static_cast<LatteTextureD3D12 *>(hostTexture);
	if (tex->GetNativeDefinition().isDepth)
		texture_clearDepthSlice(hostTexture, sliceIndex, mipIndex, true, tex->hasStencil, 0.0f, 0);
	else
		texture_clearColorSlice(hostTexture, sliceIndex, mipIndex, 0.0f, 0.0f, 0.0f, 0.0f);
}

void D3D12Renderer::texture_clearColorSlice_record(LatteTexture *hostTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a)
{
	auto *tex = static_cast<LatteTextureD3D12 *>(hostTexture);
	if (tex->GetNativeDefinition().isDepth)
	{
		cemu_assert_suspicious();
		return;
	}
	if (!tex->GetResource())
		return;
	if (tex->GetFormatInfo().rtvFormat == DXGI_FORMAT_UNKNOWN)
	{
		// compressed formats can't be cleared through an RTV
		cemuLog_logDebugOnce(LogType::Force, "D3D12: Clear of non-renderable texture format {:04x} ignored", (uint32)tex->GetNativeDefinition().format);
		return;
	}
	TransitionTexture(tex, mipIndex, 1, sliceIndex, 1, D3D12_RESOURCE_STATE_RENDER_TARGET);
	FlushBarriers();
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = _CreateTempRTV(this, tex, sliceIndex, mipIndex);
	const float color[4] = {r, g, b, a};
	m_cmdList->ClearRenderTargetView(rtv, color, 0, nullptr);
	m_stagingRTVHeap->Free(rtv); // RTV descriptors are consumed at record time
	m_state.renderTargetsDirty = true;
	m_hasRecordedWork = true;
}

void D3D12Renderer::texture_clearDepthSlice_record(LatteTexture *hostTexture, uint32 sliceIndex, sint32 mipIndex, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue)
{
	auto *tex = static_cast<LatteTextureD3D12 *>(hostTexture);
	if (!tex->GetNativeDefinition().isDepth)
	{
		cemu_assert_suspicious();
		return;
	}
	if (!tex->GetResource())
		return;
	D3D12_CLEAR_FLAGS flags = (D3D12_CLEAR_FLAGS)0;
	if (clearDepth)
		flags |= D3D12_CLEAR_FLAG_DEPTH;
	if (clearStencil && tex->GetFormatInfo().hasStencil)
		flags |= D3D12_CLEAR_FLAG_STENCIL;
	if (flags == 0)
		return;
	TransitionTexture(tex, mipIndex, 1, sliceIndex, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	FlushBarriers();
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = _CreateTempDSV(this, tex, sliceIndex, mipIndex);
	m_cmdList->ClearDepthStencilView(dsv, flags, std::clamp(depthValue, 0.0f, 1.0f), (UINT8)stencilValue, 0, nullptr);
	m_stagingDSVHeap->Free(dsv);
	m_state.renderTargetsDirty = true;
	m_hasRecordedWork = true;
}

void D3D12Renderer::texture_copyImageSubData_record(LatteTexture *src, sint32 srcMip, sint32 effectiveSrcX, sint32 effectiveSrcY, sint32 srcSlice, LatteTexture *dst, sint32 dstMip, sint32 effectiveDstX, sint32 effectiveDstY, sint32 dstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight, sint32 srcDepth)
{
	auto *srcTex = static_cast<LatteTextureD3D12 *>(src);
	auto *dstTex = static_cast<LatteTextureD3D12 *>(dst);
	if (!srcTex->GetResource() || !dstTex->GetResource())
		return;
	const auto &srcFmt = srcTex->GetFormatInfo();
	const auto &dstFmt = dstTex->GetFormatInfo();
	const bool srcIs3D = srcTex->Is3DTexture();
	const bool dstIs3D = dstTex->Is3DTexture();

	TransitionTexture(srcTex, srcMip, 1, srcIs3D ? 0 : srcSlice, srcIs3D ? 1 : srcDepth, D3D12_RESOURCE_STATE_COPY_SOURCE);
	TransitionTexture(dstTex, dstMip, 1, dstIs3D ? 0 : dstSlice, dstIs3D ? 1 : srcDepth, D3D12_RESOURCE_STATE_COPY_DEST);
	FlushBarriers();

	const bool sameFamily = D3D12Format::GetTypelessFormat(srcTex->GetDesc().Format) == D3D12Format::GetTypelessFormat(dstTex->GetDesc().Format);
	const bool planeCompatible = srcFmt.hasStencil == dstFmt.hasStencil;
	const uint32 blockDimSrc = srcFmt.isCompressed ? 4 : 1;
	const uint32 blockDimDst = dstFmt.isCompressed ? 4 : 1;

	// Keep the copy inside both mips. D3D12 records an out of range box without complaint and then fails the whole command
	// list with E_INVALIDARG on Close (seen on the MK8 title screen), which takes the emulator down
	{
		const auto mipExtent = [](LatteTextureD3D12 *tex, sint32 mip, uint32 blockDim, bool height) {
			const uint64 full = height ? tex->GetDesc().Height : tex->GetDesc().Width;
			return (sint32)AlignUp<uint64>(std::max<uint64>(1, full >> mip), blockDim);
		};
		const sint32 width = std::min({effectiveCopyWidth, mipExtent(srcTex, srcMip, blockDimSrc, false) - effectiveSrcX, mipExtent(dstTex, dstMip, blockDimDst, false) - effectiveDstX});
		const sint32 height = std::min({effectiveCopyHeight, mipExtent(srcTex, srcMip, blockDimSrc, true) - effectiveSrcY, mipExtent(dstTex, dstMip, blockDimDst, true) - effectiveDstY});
		if (width != effectiveCopyWidth || height != effectiveCopyHeight || effectiveSrcX < 0 || effectiveSrcY < 0 || effectiveDstX < 0 || effectiveDstY < 0)
		{
			static uint32 s_logged = 0;
			if (s_logged++ < 20)
				cemuLog_log(LogType::Force, "D3D12: Texture copy {}x{} at {},{} -> {},{} (mips {}/{}) outside a {}x{} -> {}x{} texture, clamped to {}x{}", effectiveCopyWidth, effectiveCopyHeight,
							effectiveSrcX, effectiveSrcY, effectiveDstX, effectiveDstY, srcMip, dstMip, srcTex->GetDesc().Width, srcTex->GetDesc().Height, dstTex->GetDesc().Width, dstTex->GetDesc().Height, width, height);
			if (width <= 0 || height <= 0 || effectiveSrcX < 0 || effectiveSrcY < 0 || effectiveDstX < 0 || effectiveDstY < 0)
				return;
			effectiveCopyWidth = width;
			effectiveCopyHeight = height;
		}
	}

	for (sint32 d = 0; d < srcDepth; d++)
	{
		const uint32 sSlice = srcSlice + d;
		const uint32 dSlice = dstSlice + d;
		const auto sliceCount = [](LatteTextureD3D12 *tex, sint32 mip) {
			const uint32 n = tex->GetDesc().DepthOrArraySize;
			return tex->Is3DTexture() ? std::max<uint32>(1, n >> mip) : n;
		};
		if (sSlice >= sliceCount(srcTex, srcMip) || dSlice >= sliceCount(dstTex, dstMip))
		{
			cemuLog_logOnce(LogType::Force, "D3D12: Texture copy slice {} -> {} outside the texture, skipped", sSlice, dSlice);
			continue;
		}
		if (sameFamily && planeCompatible)
		{
			for (uint32 plane = 0; plane < srcTex->GetPlaneCount(); plane++)
			{
				D3D12_TEXTURE_COPY_LOCATION s{}, t{};
				s.pResource = srcTex->GetResource();
				s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				s.SubresourceIndex = srcTex->GetSubresourceIndex(srcMip, sSlice, plane);
				t.pResource = dstTex->GetResource();
				t.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				t.SubresourceIndex = dstTex->GetSubresourceIndex(dstMip, dSlice, plane);
				D3D12_BOX box;
				box.left = effectiveSrcX;
				box.top = effectiveSrcY;
				box.front = srcIs3D ? sSlice : 0;
				box.right = effectiveSrcX + effectiveCopyWidth;
				box.bottom = effectiveSrcY + effectiveCopyHeight;
				box.back = box.front + 1;
				m_cmdList->CopyTextureRegion(&t, effectiveDstX, effectiveDstY, dstIs3D ? dSlice : 0, &s, &box);
			}
			continue;
		}
		// different format families (e.g. depth -> color or RGBA8 -> R32). Bounce through a buffer, which reinterprets
		// the bits like Vulkan's vkCmdCopyImage does for size-compatible formats
		const uint32 srcBpb = D3D12Format::GetBytesPerBlock(srcTex->GetDesc().Format);
		const uint32 dstBpb = D3D12Format::GetBytesPerBlock(dstTex->GetDesc().Format);
		if (srcBpb != dstBpb || srcFmt.hasStencil || dstFmt.hasStencil)
		{
			cemuLog_logDebugOnce(LogType::Force, "D3D12: Unsupported texture copy between formats {:04x} and {:04x}", (uint32)srcTex->GetNativeDefinition().format, (uint32)dstTex->GetNativeDefinition().format);
			continue;
		}
		const uint32 widthBlocks = (effectiveCopyWidth + blockDimSrc - 1) / blockDimSrc;
		const uint32 heightBlocks = (effectiveCopyHeight + blockDimSrc - 1) / blockDimSrc;
		const uint32 rowPitch = AlignUp<uint32>(widthBlocks * srcBpb, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
		const uint32 bufferSize = rowPitch * heightBlocks;
		if (m_bufferCacheScratchSize < bufferSize)
		{
			ReleaseResourceDeferred(std::move(m_bufferCacheScratch));
			m_bufferCacheScratchSize = AlignUp<uint32>(bufferSize, 1024 * 1024);
			D3D12_HEAP_PROPERTIES heap = D3D12_HeapProps(D3D12_HEAP_TYPE_DEFAULT);
			D3D12_RESOURCE_DESC bd = D3D12_BufferDesc((uint64)m_bufferCacheScratchSize + D3D12Const::kMaxConstantBufferBytes);
			D3D12_ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_bufferCacheScratch)), "CreateCommittedResource (scratch)");
			m_bufferCacheScratchState = D3D12_RESOURCE_STATE_COMMON;
		}
		TransitionResource(m_bufferCacheScratch.Get(), m_bufferCacheScratchState, D3D12_RESOURCE_STATE_COPY_DEST);
		FlushBarriers();
		D3D12_TEXTURE_COPY_LOCATION s{}, buf{};
		s.pResource = srcTex->GetResource();
		s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		s.SubresourceIndex = srcTex->GetSubresourceIndex(srcMip, sSlice, 0);
		buf.pResource = m_bufferCacheScratch.Get();
		buf.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		buf.PlacedFootprint.Offset = 0;
		buf.PlacedFootprint.Footprint.Format = D3D12Format::GetTypelessFormat(srcTex->GetDesc().Format);
		buf.PlacedFootprint.Footprint.Width = widthBlocks * blockDimSrc;
		buf.PlacedFootprint.Footprint.Height = heightBlocks * blockDimSrc;
		buf.PlacedFootprint.Footprint.Depth = 1;
		buf.PlacedFootprint.Footprint.RowPitch = rowPitch;
		D3D12_BOX box{(UINT)effectiveSrcX, (UINT)effectiveSrcY, srcIs3D ? sSlice : 0, (UINT)(effectiveSrcX + widthBlocks * blockDimSrc), (UINT)(effectiveSrcY + heightBlocks * blockDimSrc), (srcIs3D ? sSlice : 0) + 1};
		m_cmdList->CopyTextureRegion(&buf, 0, 0, 0, &s, &box);
		TransitionResource(m_bufferCacheScratch.Get(), m_bufferCacheScratchState, D3D12_RESOURCE_STATE_COPY_SOURCE);
		FlushBarriers();
		D3D12_TEXTURE_COPY_LOCATION t{};
		t.pResource = dstTex->GetResource();
		t.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		t.SubresourceIndex = dstTex->GetSubresourceIndex(dstMip, dSlice, 0);
		buf.PlacedFootprint.Footprint.Format = D3D12Format::GetTypelessFormat(dstTex->GetDesc().Format);
		buf.PlacedFootprint.Footprint.Width = widthBlocks * blockDimDst;
		buf.PlacedFootprint.Footprint.Height = heightBlocks * blockDimDst;
		m_cmdList->CopyTextureRegion(&t, effectiveDstX, effectiveDstY, dstIs3D ? dSlice : 0, &buf, nullptr);
	}
	m_hasRecordedWork = true;
}

LatteTextureReadbackInfo *D3D12Renderer::texture_createReadback(LatteTextureView *textureView)
{
	auto *result = new LatteTextureReadbackInfoD3D12(this, textureView);
	if (result->GetImageSize() == 0)
	{
		delete result;
		return nullptr;
	}
	return result;
}

/* --- buffer cache --- */

static constexpr D3D12_RESOURCE_STATES kBufferCacheReadState = (D3D12_RESOURCE_STATES)((UINT)D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | (UINT)D3D12_RESOURCE_STATE_INDEX_BUFFER | (UINT)D3D12_RESOURCE_STATE_COPY_SOURCE);

void D3D12Renderer::bufferCache_init(const sint32 bufferSize)
{
	m_pipeline.CheckFront();
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::BufferInit;
	packet.words[0] = bufferSize;
	packet.words[1] = AlignUp<uint32>(LatteStreamout_GetRingBufferSize() * 4, 64 * 1024);
	ExecuteBackend(LattePipeline::Opcode::Create, packet);
}

void D3D12Renderer::bufferCache_init_record(uint32 bufferSize, uint32 streamoutSize)
{
	// A recreated resource may reuse its former COM address. Force persistent
	// source descriptors and prior table contents to be refreshed in that case.
	for (auto &uav : m_stageUAVCache)
		uav.resource = nullptr;
	for (auto &table : m_lastViewTable)
		table.Clear();
	for (auto &cache : m_viewTableCache)
		cache.Clear();
	memset(m_state.currentRootCBVs, 0, sizeof(m_state.currentRootCBVs));

	ReleaseResourceDeferred(std::move(m_bufferCache));
	ReleaseResourceDeferred(std::move(m_xfbRingBuffer));
	m_bufferCacheSize = (uint32)bufferSize;
	D3D12_HEAP_PROPERTIES heap = D3D12_HeapProps(D3D12_HEAP_TYPE_DEFAULT);
	D3D12_RESOURCE_DESC desc = D3D12_BufferDesc((uint64)bufferSize + D3D12Const::kMaxConstantBufferBytes);
	D3D12_ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_bufferCache)), "CreateCommittedResource (buffer cache)");
	D3D12_SetDebugName(m_bufferCache.Get(), "BufferCache");
	m_bufferCacheState = D3D12_RESOURCE_STATE_COMMON;

	m_xfbRingBufferSize = streamoutSize; // same headroom as the Metal backend
	D3D12_RESOURCE_DESC xfbDesc = D3D12_BufferDesc(m_xfbRingBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	D3D12_ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &xfbDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_xfbRingBuffer)), "CreateCommittedResource (XFB)");
	D3D12_SetDebugName(m_xfbRingBuffer.Get(), "XfbRingBuffer");
	m_xfbRingBufferState = D3D12_RESOURCE_STATE_COMMON;
}

void D3D12Renderer::bufferCache_upload(uint8 *buffer, sint32 size, uint32 bufferOffset)
{
	m_pipeline.CheckFront();
	if (size <= 0)
		return;
	if (!m_pipeline.Queued())
	{
		bufferCache_upload_record(buffer, static_cast<uint32>(size), bufferOffset);
		return;
	}
	uint32 copied = 0;
	while (copied < static_cast<uint32>(size))
	{
		const uint32 bytes = std::min<uint32>(static_cast<uint32>(size) - copied, static_cast<uint32>(m_operations.Staging().Capacity()));
		BackendPacket packet{};
		packet.kind = BackendPacket::Kind::BufferUpload;
		packet.words[0] = bufferOffset + copied;
		packet.bytes = StageBytes(buffer + copied, bytes);
		ExecuteBackend(LattePipeline::Opcode::BufferUpload, packet);
		copied += bytes;
	}
}

void D3D12Renderer::bufferCache_upload_record(const void *buffer, uint32 size, uint32 bufferOffset)
{
	LatteProfiler::Scope profileScope(LatteProfiler::Slot::BufferSync);
	D3D12UploadAllocation upload = AllocateUpload((uint64)size, 16);
	memcpy(upload.cpuPtr, buffer, size);
	TransitionResource(m_bufferCache.Get(), m_bufferCacheState, D3D12_RESOURCE_STATE_COPY_DEST);
	FlushBarriers();
	m_cmdList->CopyBufferRegion(m_bufferCache.Get(), bufferOffset, upload.resource, upload.offset, (UINT64)size);
	LatteProfiler::Add(LatteProfiler::Counter::BufferBytes, size);
	m_hasRecordedWork = true;
}

void D3D12Renderer::bufferCache_copy_record(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
	// copies within the same buffer need the resource in COPY_SOURCE and COPY_DEST at the same time, which D3D12 does
	// not allow. Bounce through the scratch buffer
	if (m_bufferCacheScratchSize < size)
	{
		ReleaseResourceDeferred(std::move(m_bufferCacheScratch));
		m_bufferCacheScratchSize = AlignUp<uint32>(size, 1024 * 1024);
		D3D12_HEAP_PROPERTIES heap = D3D12_HeapProps(D3D12_HEAP_TYPE_DEFAULT);
		D3D12_RESOURCE_DESC bd = D3D12_BufferDesc((uint64)m_bufferCacheScratchSize + D3D12Const::kMaxConstantBufferBytes);
		D3D12_ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_bufferCacheScratch)), "CreateCommittedResource (scratch)");
		m_bufferCacheScratchState = D3D12_RESOURCE_STATE_COMMON;
	}
	TransitionResource(m_bufferCache.Get(), m_bufferCacheState, kBufferCacheReadState);
	TransitionResource(m_bufferCacheScratch.Get(), m_bufferCacheScratchState, D3D12_RESOURCE_STATE_COPY_DEST);
	FlushBarriers();
	m_cmdList->CopyBufferRegion(m_bufferCacheScratch.Get(), 0, m_bufferCache.Get(), srcOffset, size);
	TransitionResource(m_bufferCache.Get(), m_bufferCacheState, D3D12_RESOURCE_STATE_COPY_DEST);
	TransitionResource(m_bufferCacheScratch.Get(), m_bufferCacheScratchState, D3D12_RESOURCE_STATE_COPY_SOURCE);
	FlushBarriers();
	m_cmdList->CopyBufferRegion(m_bufferCache.Get(), dstOffset, m_bufferCacheScratch.Get(), 0, size);
	m_hasRecordedWork = true;
}

void D3D12Renderer::bufferCache_copyStreamoutToMainBuffer_record(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
	TransitionResource(m_xfbRingBuffer.Get(), m_xfbRingBufferState, D3D12_RESOURCE_STATE_COPY_SOURCE);
	TransitionResource(m_bufferCache.Get(), m_bufferCacheState, D3D12_RESOURCE_STATE_COPY_DEST);
	FlushBarriers();
	m_cmdList->CopyBufferRegion(m_bufferCache.Get(), dstOffset, m_xfbRingBuffer.Get(), srcOffset, size);
	m_hasRecordedWork = true;
}

void D3D12Renderer::buffer_bindVertexBuffers(std::span<BindBufferParam> bindings)
{
	m_pipeline.CheckFront();
	if (!m_pipeline.Queued())
	{
		buffer_bindVertexBuffers_record(bindings);
		return;
	}
	for (const auto &b : bindings)
		m_frontBindings.vertices[b.index] = {b.bindOffset, b.bindSize, b.stride, true};
}

void D3D12Renderer::buffer_bindVertexBuffers_record(std::span<const BindBufferParam> bindings)
{
	const D3D12_GPU_VIRTUAL_ADDRESS base = m_bufferCache->GetGPUVirtualAddress();
	for (const auto &b : bindings)
	{
		cemu_assert_debug(b.index < LATTE_MAX_VERTEX_BUFFERS);
		D3D12_VERTEX_BUFFER_VIEW view;
		view.BufferLocation = base + b.bindOffset;
		view.SizeInBytes = std::min<uint32>(b.bindSize, m_bufferCacheSize - b.bindOffset);
		view.StrideInBytes = b.stride;
		auto &cur = m_state.vertexBuffers[b.index];
		if (cur.BufferLocation != view.BufferLocation || cur.SizeInBytes != view.SizeInBytes || cur.StrideInBytes != view.StrideInBytes)
		{
			cur = view;
			m_state.vertexBufferDirtyMask |= (1u << b.index);
		}
	}
}

void D3D12Renderer::buffer_bindUniformBuffer(LatteConst::ShaderType shaderType, uint32 bufferIndex, uint32 offset, uint32 size)
{
	cemu_assert_debug(bufferIndex < LATTE_NUM_MAX_UNIFORM_BUFFERS);
	m_pipeline.CheckFront();
	const auto stage = static_cast<uint32>(D3D12StageFromLatte(shaderType));
	if (!m_pipeline.Queued())
		buffer_bindUniformBuffer_record(stage, bufferIndex, offset, size);
	else
		m_frontBindings.uniforms[stage][bufferIndex] = {offset, size};
}

void D3D12Renderer::buffer_bindUniformBuffer_record(uint32 stageIndex, uint32 bufferIndex, uint32 offset, uint32 size)
{
	auto &stage = m_state.stage[stageIndex];
	if (size == 0)
	{
		stage.uniformBufferAddress[bufferIndex] = 0;
		stage.uniformBufferSize[bufferIndex] = 0;
		return;
	}
	stage.uniformBufferAddress[bufferIndex] = GetAlignedUniformBufferAddress(offset, size);
	stage.uniformBufferSize[bufferIndex] = size;
}

D3D12_GPU_VIRTUAL_ADDRESS D3D12Renderer::GetAlignedUniformBufferAddress(uint32 cacheOffset, uint32 size)
{
	// Backing resources include a 64-KiB tail beyond the logical cache/scratch
	// capacity, so even a root cbuffer at the final aligned offset stays in-resource.
	// CBVs must start at 256 byte boundaries. GX2 requires the same alignment for uniform blocks and the buffer cache
	// preserves it, so this is the common case
	if ((cacheOffset % D3D12Const::kCBVAlignment) == 0 && (uint64)cacheOffset + AlignUp<uint32>(size, D3D12Const::kCBVAlignment) <= m_bufferCacheSize)
		return m_bufferCache->GetGPUVirtualAddress() + cacheOffset;
	// otherwise copy the data to an aligned location in the scratch buffer
	cemuLog_logDebugOnce(LogType::Force, "D3D12: Unaligned uniform buffer at cache offset {:08x}", cacheOffset);
	const uint32 alignedSize = AlignUp<uint32>(size, D3D12Const::kCBVAlignment);
	if (!m_bufferCacheScratch || m_bufferCacheScratchSize < alignedSize * 2)
	{
		ReleaseResourceDeferred(std::move(m_bufferCacheScratch));
		m_bufferCacheScratchSize = std::max<uint32>(AlignUp<uint32>(alignedSize * 2, 1024 * 1024), 4 * 1024 * 1024);
		D3D12_HEAP_PROPERTIES heap = D3D12_HeapProps(D3D12_HEAP_TYPE_DEFAULT);
		D3D12_RESOURCE_DESC bd = D3D12_BufferDesc((uint64)m_bufferCacheScratchSize + D3D12Const::kMaxConstantBufferBytes);
		D3D12_ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_bufferCacheScratch)), "CreateCommittedResource (scratch)");
		m_bufferCacheScratchState = D3D12_RESOURCE_STATE_COMMON;
		m_bufferCacheScratchOffset = 0;
	}
	if (m_bufferCacheScratchOffset + alignedSize > m_bufferCacheScratchSize)
	{
		// wrap around. Submit so previous users of the scratch area are ordered before the new copy
		SubmitCommandList(false);
		m_bufferCacheScratchOffset = 0;
	}
	const uint32 dstOffset = m_bufferCacheScratchOffset;
	m_bufferCacheScratchOffset += alignedSize;
	TransitionResource(m_bufferCache.Get(), m_bufferCacheState, kBufferCacheReadState);
	TransitionResource(m_bufferCacheScratch.Get(), m_bufferCacheScratchState, D3D12_RESOURCE_STATE_COPY_DEST);
	FlushBarriers();
	const uint32 copySize = std::min<uint32>(size, m_bufferCacheSize - cacheOffset);
	m_cmdList->CopyBufferRegion(m_bufferCacheScratch.Get(), dstOffset, m_bufferCache.Get(), cacheOffset, copySize);
	TransitionResource(m_bufferCacheScratch.Get(), m_bufferCacheScratchState, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
	m_hasRecordedWork = true;
	return m_bufferCacheScratch->GetGPUVirtualAddress() + dstOffset;
}

/* --- index data --- */

Renderer::IndexAllocation D3D12Renderer::indexData_reserveIndexMemory(uint32 size)
{
	if (!m_pipeline.Queued())
	{
		auto *allocation = m_indexAllocator->Allocate(size, 4);
		return allocation ? IndexAllocation{allocation->cpuPtr, allocation} : IndexAllocation{};
	}
	// IDs and conversion scratch belong to front. Ordered creation and upload
	// need no allocation-success round trip before a draw can be published.
	auto resource = std::make_unique<IndexResource>();
	if (m_indexDecodeScratch.size() < size)
		m_indexDecodeScratch.resize(size);
	resource->size = size;
	resource->id = RegisterResource(resource.get());
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::IndexCreate;
	packet.resources[0] = resource->id;
	packet.words[0] = size;
	auto *result = resource.release();
	ExecuteBackend(LattePipeline::Opcode::Create, packet);
	return {m_indexDecodeScratch.data(), result};
}

void D3D12Renderer::indexData_releaseIndexMemory(IndexAllocation &allocation)
{
	if (!m_pipeline.Queued())
	{
		m_indexAllocator->Free(static_cast<D3D12UploadHeapAllocator::Allocation *>(allocation.rendererInternal));
		allocation = {};
		return;
	}
	auto *resource = static_cast<IndexResource *>(allocation.rendererInternal);
	if (!resource)
		return;
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::IndexRelease;
	packet.resources[0] = resource->id;
	// The recorder deletes the adapter only after its final queued use. Native
	// upload storage remains fence-retired by the allocator.
	ExecuteBackend(LattePipeline::Opcode::Release, packet);
	allocation = {};
}

void D3D12Renderer::indexData_uploadIndexMemory(IndexAllocation &allocation)
{
	if (!m_pipeline.Queued())
	{
		const auto size = static_cast<D3D12UploadHeapAllocator::Allocation *>(allocation.rendererInternal)->size;
		LatteProfiler::Add(LatteProfiler::Counter::UploadBytes, size);
		LatteProfiler::Add(LatteProfiler::Counter::IndexUploadBytes, size);
		return; // Coherent write-combined memory was filled by the decoder.
	}
	auto *resource = static_cast<IndexResource *>(allocation.rendererInternal);
	uint32 copied = 0;
	while (copied < resource->size)
	{
		const uint32 bytes = std::min<uint32>(resource->size - copied, static_cast<uint32>(m_operations.Staging().Capacity()));
		BackendPacket packet{};
		packet.kind = BackendPacket::Kind::IndexUpload;
		packet.resources[0] = resource->id;
		packet.words[0] = copied;
		packet.bytes = StageBytes(static_cast<const uint8 *>(allocation.mem) + copied, bytes);
		ExecuteBackend(LattePipeline::Opcode::IndexUpload, packet);
		copied += bytes;
	}
	allocation.mem = nullptr;
}

/* --- streamout --- */

void D3D12Renderer::streamout_setupXfbBuffer(uint32 bufferIndex, sint32 ringBufferOffset, uint32 rangeAddr, uint32 rangeSize)
{
	m_frontState.streamoutBuffer[bufferIndex].enabled = true;
	m_frontState.streamoutBuffer[bufferIndex].ringBufferOffset = ringBufferOffset;
}

void D3D12Renderer::streamout_begin()
{
	// writes happen through a UAV in the vertex shader, see draw path
}

void D3D12Renderer::streamout_rendererFinishDrawcall_record()
{
	// make the UAV writes visible to the copy that follows (bufferCache_copyStreamoutToMainBuffer)
	UAVBarrier(m_xfbRingBuffer.Get());
}

/* --- screenshots --- */

void D3D12Renderer::HandleScreenshotRequest(LatteTextureView *texView, bool padView)
{
	if (!m_screenshot_requested && m_screenshot_state == ScreenshotState::None)
		return;
	if (IsPadWindowActive())
	{
		if (m_screenshot_state == ScreenshotState::Main && padView)
			return;
		if (m_screenshot_state == ScreenshotState::Pad && !padView)
			return;
		if (m_screenshot_state == ScreenshotState::None)
			m_screenshot_state = padView ? ScreenshotState::Main : ScreenshotState::Pad;
		else
			m_screenshot_state = ScreenshotState::None;
	}
	else
		m_screenshot_state = ScreenshotState::None;

	auto *view = static_cast<LatteTextureViewD3D12 *>(texView);
	auto *tex = view->GetBaseTexture();
	if (!tex->GetResource())
		return;
	if (view->GetNativeDefinition().firstMip != 0)
	{
		cemuLog_log(LogType::Force, "Failed to capture screenshot: capturing non-zero mip is not supported");
		return;
	}
	const DXGI_FORMAT typeless = D3D12Format::GetTypelessFormat(tex->GetDesc().Format);
	if (typeless != DXGI_FORMAT_R8G8B8A8_TYPELESS)
	{
		cemuLog_log(LogType::Force, "Failed to capture screenshot: texture format {:04x} is not supported by the D3D12 backend", (uint32)tex->GetNativeDefinition().format);
		return;
	}
	sint32 width, height;
	tex->GetEffectiveSize(width, height, 0);
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::Screenshot;
	packet.resources[0] = tex->recordingId;
	packet.words[0] = view->GetNativeDefinition().firstSlice;
	packet.words[1] = width;
	packet.words[2] = height;
	const auto reply = ExecuteBackend(LattePipeline::Opcode::Readback, packet);
	m_screenshotBytes.resize(reply.bytes.size());
	memcpy(m_screenshotBytes.data(), reply.bytes.data(), m_screenshotBytes.size());
	const uint32 rowPitch = AlignUp<uint32>((uint32)width * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
	const bool formatIsSRGB = tex->GetFormatInfo().srvFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	const bool dstUsesSRGB = (!padView && LatteGPUState.tvBufferUsesSRGB) || (padView && LatteGPUState.drcBufferUsesSRGB);
	std::vector<uint8> rgb((size_t)width * height * 3);
	const uint8 *src = m_screenshotBytes.data();
	for (sint32 y = 0; y < height; y++)
	{
		for (sint32 x = 0; x < width; x++)
		{
			const uint8 *p = src + (size_t)y * rowPitch + (size_t)x * 4;
			uint8 *o = rgb.data() + ((size_t)y * width + x) * 3;
			for (int c = 0; c < 3; c++)
			{
				uint8 v = p[c];
				if (formatIsSRGB && !dstUsesSRGB)
					v = SRGBComponentToRGB(v);
				else if (!formatIsSRGB && dstUsesSRGB)
					v = RGBComponentToSRGB(v);
				o[c] = v;
			}
		}
	}
	SaveScreenshot(rgb, width, height, !padView);
}

/* --- ordered backend operations (front prepare / back record) --- */

void D3D12Renderer::texture_clearColorSlice(LatteTexture *hostTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a)
{
	if (!m_pipeline.Queued() || m_operations.Recording())
	{
		texture_clearColorSlice_record(hostTexture, sliceIndex, mipIndex, r, g, b, a);
		return;
	}
	m_pipeline.CheckFront();
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::texture_clearColorSlice;
	packet.resources[0] = static_cast<LatteTextureD3D12 *>(hostTexture)->recordingId;
	packet.words[0] = static_cast<uint32>(sliceIndex);
	packet.words[1] = static_cast<uint32>(mipIndex);
	packet.words[2] = std::bit_cast<uint32>(r);
	packet.words[3] = std::bit_cast<uint32>(g);
	packet.words[4] = std::bit_cast<uint32>(b);
	packet.words[5] = std::bit_cast<uint32>(a);
	ExecuteBackend(LattePipeline::Opcode::Clear, packet);
}

void D3D12Renderer::texture_clearDepthSlice(LatteTexture *hostTexture, uint32 sliceIndex, sint32 mipIndex, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue)
{
	if (!m_pipeline.Queued() || m_operations.Recording())
	{
		texture_clearDepthSlice_record(hostTexture, sliceIndex, mipIndex, clearDepth, clearStencil, depthValue, stencilValue);
		return;
	}
	m_pipeline.CheckFront();
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::texture_clearDepthSlice;
	packet.resources[0] = static_cast<LatteTextureD3D12 *>(hostTexture)->recordingId;
	packet.words[0] = static_cast<uint32>(sliceIndex);
	packet.words[1] = static_cast<uint32>(mipIndex);
	packet.words[2] = static_cast<uint32>(clearDepth);
	packet.words[3] = static_cast<uint32>(clearStencil);
	packet.words[4] = std::bit_cast<uint32>(depthValue);
	packet.words[5] = static_cast<uint32>(stencilValue);
	ExecuteBackend(LattePipeline::Opcode::Clear, packet);
}

void D3D12Renderer::texture_copyImageSubData(LatteTexture *src, sint32 srcMip, sint32 effectiveSrcX, sint32 effectiveSrcY, sint32 srcSlice, LatteTexture *dst, sint32 dstMip, sint32 effectiveDstX, sint32 effectiveDstY, sint32 dstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight, sint32 srcDepth)
{
	if (!m_pipeline.Queued() || m_operations.Recording())
	{
		texture_copyImageSubData_record(src, srcMip, effectiveSrcX, effectiveSrcY, srcSlice, dst, dstMip, effectiveDstX, effectiveDstY, dstSlice, effectiveCopyWidth, effectiveCopyHeight, srcDepth);
		return;
	}
	m_pipeline.CheckFront();
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::texture_copyImageSubData;
	packet.resources[0] = static_cast<LatteTextureD3D12 *>(src)->recordingId;
	packet.words[0] = static_cast<uint32>(srcMip);
	packet.words[1] = static_cast<uint32>(effectiveSrcX);
	packet.words[2] = static_cast<uint32>(effectiveSrcY);
	packet.words[3] = static_cast<uint32>(srcSlice);
	packet.resources[1] = static_cast<LatteTextureD3D12 *>(dst)->recordingId;
	packet.words[4] = static_cast<uint32>(dstMip);
	packet.words[5] = static_cast<uint32>(effectiveDstX);
	packet.words[6] = static_cast<uint32>(effectiveDstY);
	packet.words[7] = static_cast<uint32>(dstSlice);
	packet.words[8] = static_cast<uint32>(effectiveCopyWidth);
	packet.words[9] = static_cast<uint32>(effectiveCopyHeight);
	packet.words[10] = static_cast<uint32>(srcDepth);
	ExecuteBackend(LattePipeline::Opcode::Copy, packet);
}

void D3D12Renderer::bufferCache_copy(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
	if (!m_pipeline.Queued() || m_operations.Recording())
	{
		bufferCache_copy_record(srcOffset, dstOffset, size);
		return;
	}
	m_pipeline.CheckFront();
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::bufferCache_copy;
	packet.words[0] = static_cast<uint32>(srcOffset);
	packet.words[1] = static_cast<uint32>(dstOffset);
	packet.words[2] = static_cast<uint32>(size);
	ExecuteBackend(LattePipeline::Opcode::Copy, packet);
}

void D3D12Renderer::bufferCache_copyStreamoutToMainBuffer(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
	if (!m_pipeline.Queued() || m_operations.Recording())
	{
		bufferCache_copyStreamoutToMainBuffer_record(srcOffset, dstOffset, size);
		return;
	}
	m_pipeline.CheckFront();
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::bufferCache_copyStreamoutToMainBuffer;
	packet.words[0] = static_cast<uint32>(srcOffset);
	packet.words[1] = static_cast<uint32>(dstOffset);
	packet.words[2] = static_cast<uint32>(size);
	ExecuteBackend(LattePipeline::Opcode::Copy, packet);
}

void D3D12Renderer::streamout_rendererFinishDrawcall()
{
	if (!m_pipeline.Queued() || m_operations.Recording())
	{
		streamout_rendererFinishDrawcall_record();
		return;
	}
	m_pipeline.CheckFront();
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::streamout_rendererFinishDrawcall;
	ExecuteBackend(LattePipeline::Opcode::Bind, packet);
}

void D3D12Renderer::SubmitCommandList(bool waitIdle)
{
	if (!m_pipeline.Queued() || m_operations.Recording())
	{
		SubmitCommandList_record(waitIdle);
		return;
	}
	m_pipeline.CheckFront();
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::SubmitCommandList;
	packet.words[0] = static_cast<uint32>(waitIdle);
	ExecuteBackend(LattePipeline::Opcode::Submit, packet);
}

void D3D12Renderer::Flush(bool waitIdle)
{
	if (!m_pipeline.Queued() || m_operations.Recording())
	{
		Flush_record(waitIdle);
		return;
	}
	m_pipeline.CheckFront();
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::Flush;
	packet.words[0] = static_cast<uint32>(waitIdle);
	ExecuteBackend(LattePipeline::Opcode::Submit, packet);
}

void D3D12Renderer::NotifyLatteCommandProcessorIdle()
{
	if (!m_pipeline.Queued() || m_operations.Recording())
	{
		NotifyLatteCommandProcessorIdle_record();
		return;
	}
	m_pipeline.CheckFront();
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::NotifyLatteCommandProcessorIdle;
	ExecuteBackend(LattePipeline::Opcode::Control, packet);
}

void D3D12Renderer::SwapBuffers(bool swapTV, bool swapDRC)
{
	if (!m_pipeline.Queued() || m_operations.Recording())
	{
		SwapBuffers_record(swapTV, swapDRC);
		return;
	}
	m_pipeline.CheckFront();

	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::SwapBuffers;
	packet.words[0] = static_cast<uint32>(swapTV);
	packet.words[1] = static_cast<uint32>(swapDRC);
	packet.words[2] = GetD3D12Settings().vsync;
	packet.words[3] = GetD3D12Settings().frameInterpolation;
	ExecuteBackend(LattePipeline::Opcode::Present, packet);
	if (swapTV && m_pipelineCache)
		m_pipelineCache->TickBackgroundPreload();
	m_pipeline.EndFrame();
}

void D3D12Renderer::ClearColorbuffer(bool padView)
{
	if (!m_pipeline.Queued() || m_operations.Recording())
	{
		ClearColorbuffer_record(padView);
		return;
	}
	m_pipeline.CheckFront();
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::ClearColorbuffer;
	packet.words[0] = static_cast<uint32>(padView);
	ExecuteBackend(LattePipeline::Opcode::Clear, packet);
}

void D3D12Renderer::DrawEmptyFrame(bool mainWindow)
{
	if (!m_pipeline.Queued() || m_operations.Recording())
	{
		DrawEmptyFrame_record(mainWindow);
		return;
	}
	m_pipeline.CheckFront();
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::DrawEmptyFrame;
	packet.words[0] = static_cast<uint32>(mainWindow);
	packet.words[1] = GetD3D12Settings().vsync;
	packet.words[2] = GetD3D12Settings().frameInterpolation;
	ExecuteBackend(LattePipeline::Opcode::Present, packet);
	if (mainWindow && m_pipelineCache)
		m_pipelineCache->TickBackgroundPreload();
	m_pipeline.EndFrame();
}

void D3D12Renderer::occlusionQuery_flush()
{
	if (!m_pipeline.Queued() || m_operations.Recording())
	{
		occlusionQuery_flush_record();
		return;
	}
	m_pipeline.CheckFront();
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::occlusionQuery_flush;
	ExecuteBackend(LattePipeline::Opcode::Query, packet);
}

void D3D12Renderer::occlusionQuery_updateState()
{
	if (!m_pipeline.Queued() || m_operations.Recording())
	{
		occlusionQuery_updateState_record();
		return;
	}
	m_pipeline.CheckFront();
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::occlusionQuery_updateState;
	ExecuteBackend(LattePipeline::Opcode::Query, packet);
}

LattePipeline::Completion D3D12Renderer::ExecuteBackend(LattePipeline::Opcode opcode, const BackendPacket &packet)
{
	m_pipeline.CheckFront();
	using Kind = BackendPacket::Kind;
	const bool needsReply = (opcode == LattePipeline::Opcode::Create && packet.kind != Kind::IndexCreate) ||
							(opcode == LattePipeline::Opcode::Release && packet.kind != Kind::IndexRelease) ||
							opcode == LattePipeline::Opcode::Present || opcode == LattePipeline::Opcode::Shutdown ||
							opcode == LattePipeline::Opcode::Readback || opcode == LattePipeline::Opcode::Submit ||
							packet.kind == Kind::UIAcquire || packet.kind == Kind::UIRender || packet.kind == Kind::BeginFrame ||
							packet.kind == Kind::DebugSnapshot || packet.kind == Kind::SurfaceCopy || packet.kind == Kind::occlusionQuery_flush;
	const auto sequence = m_operations.Execute<BackendPacket, &D3D12Renderer::RecordBackend>(m_pipeline, opcode, packet, *this, needsReply);
	LattePipeline::Completion reply;
	if (needsReply)
	{
		if (m_pipeline.Threaded() && opcode == LattePipeline::Opcode::Create)
			LatteProfiler::Add(LatteProfiler::Counter::ResourceCreateReplies);
		m_operations.Wait(sequence);
		reply = m_operations.TakeReply(sequence);
	}
	// Logical destructors publish Release while their front cache entry can
	// still be on an outer readback/query traversal. Defer cache callbacks
	// until that traversal has removed the adapter; transport polling is safe.
	if (opcode != LattePipeline::Opcode::Release)
		ApplyFrontCompletions();
	UpdatePipelineProfile();
	return reply;
}

void D3D12Renderer::RecordBackend(const BackendPacket &packet, const LattePipeline::Arena &arena)
{
	m_pipeline.CheckBack();
	UpdatePipelineProfile();
	switch (packet.kind)
	{
	case BackendPacket::Kind::texture_clearColorSlice:
		texture_clearColorSlice_record(m_resources.Get<LatteTextureD3D12>(packet.resources[0]), static_cast<sint32>(packet.words[0]), static_cast<sint32>(packet.words[1]), std::bit_cast<float>(packet.words[2]), std::bit_cast<float>(packet.words[3]), std::bit_cast<float>(packet.words[4]), std::bit_cast<float>(packet.words[5]));
		break;
	case BackendPacket::Kind::texture_clearDepthSlice:
		texture_clearDepthSlice_record(m_resources.Get<LatteTextureD3D12>(packet.resources[0]), static_cast<uint32>(packet.words[0]), static_cast<sint32>(packet.words[1]), static_cast<bool>(packet.words[2]), static_cast<bool>(packet.words[3]), std::bit_cast<float>(packet.words[4]), static_cast<uint32>(packet.words[5]));
		break;
	case BackendPacket::Kind::texture_copyImageSubData:
		texture_copyImageSubData_record(m_resources.Get<LatteTextureD3D12>(packet.resources[0]), static_cast<sint32>(packet.words[0]), static_cast<sint32>(packet.words[1]), static_cast<sint32>(packet.words[2]), static_cast<sint32>(packet.words[3]), m_resources.Get<LatteTextureD3D12>(packet.resources[1]), static_cast<sint32>(packet.words[4]), static_cast<sint32>(packet.words[5]), static_cast<sint32>(packet.words[6]), static_cast<sint32>(packet.words[7]), static_cast<sint32>(packet.words[8]), static_cast<sint32>(packet.words[9]), static_cast<sint32>(packet.words[10]));
		break;
	case BackendPacket::Kind::bufferCache_copy:
		bufferCache_copy_record(static_cast<uint32>(packet.words[0]), static_cast<uint32>(packet.words[1]), static_cast<uint32>(packet.words[2]));
		break;
	case BackendPacket::Kind::bufferCache_copyStreamoutToMainBuffer:
		bufferCache_copyStreamoutToMainBuffer_record(static_cast<uint32>(packet.words[0]), static_cast<uint32>(packet.words[1]), static_cast<uint32>(packet.words[2]));
		break;
	case BackendPacket::Kind::streamout_rendererFinishDrawcall:
		streamout_rendererFinishDrawcall_record();
		break;
	case BackendPacket::Kind::SubmitCommandList:
		SubmitCommandList_record(static_cast<bool>(packet.words[0]));
		break;
	case BackendPacket::Kind::Flush:
		Flush_record(static_cast<bool>(packet.words[0]));
		break;
	case BackendPacket::Kind::NotifyLatteCommandProcessorIdle:
		NotifyLatteCommandProcessorIdle_record();
		break;
	case BackendPacket::Kind::SwapBuffers:
		m_presentVsync = static_cast<int>(packet.words[2]);
		m_presentInterpolation = packet.words[3] != 0;
		SwapBuffers_record(static_cast<bool>(packet.words[0]), static_cast<bool>(packet.words[1]));
		break;
	case BackendPacket::Kind::ClearColorbuffer:
		ClearColorbuffer_record(static_cast<bool>(packet.words[0]));
		break;
	case BackendPacket::Kind::DrawEmptyFrame:
		m_presentVsync = static_cast<int>(packet.words[1]);
		m_presentInterpolation = packet.words[2] != 0;
		DrawEmptyFrame_record(static_cast<bool>(packet.words[0]));
		break;
	case BackendPacket::Kind::occlusionQuery_flush:
		occlusionQuery_flush_record();
		break;
	case BackendPacket::Kind::occlusionQuery_updateState:
		occlusionQuery_updateState_record();
		break;
	case BackendPacket::Kind::RegistryRetire:
		m_resources.Release(packet.resources[0]);
		break;
	case BackendPacket::Kind::UICreate:
		m_operations.Result().words[0] = reinterpret_cast<uintptr_t>(m_imgui->CreateTexture(nullptr, packet.words[0], packet.words[1]));
		break;
	case BackendPacket::Kind::UIUpload:
		m_imgui->UploadTexture(LattePipeline::OpaqueId(packet.resources[0]), reinterpret_cast<const uint8 *>(arena.Read(packet.bytes).data()), packet.words[0], packet.words[1], packet.words[2], packet.words[3] != 0);
		break;
	case BackendPacket::Kind::UIRelease:
		if (m_imgui)
			m_imgui->DestroyTexture(LattePipeline::OpaqueId(packet.resources[0]));
		break;
	case BackendPacket::Kind::UIRender:
		ui_record(packet);
		break;
	case BackendPacket::Kind::UIAcquire:
		m_operations.Result().words[0] = AcquireBackbuffer(packet.words[0] != 0);
		break;
	case BackendPacket::Kind::Screenshot:
		capture_record(packet);
		break;
	case BackendPacket::Kind::EndSequence:
		if (m_drawsInCommandList >= kDrawsPerSubmission || packet.words[0] || m_uploadRing->GetRecordingOverflowBytes() > 0)
			SubmitCommandList(false);
		break;
	case BackendPacket::Kind::ServicePresent:
		ServicePendingPresent(false);
		break;
	case BackendPacket::Kind::BeginFrame:
		m_operations.Result().words[0] = BeginFrame_record(packet.words[0] != 0);
		break;
	case BackendPacket::Kind::ShutdownDrain:
		WaitForIdle();
		break;
	case BackendPacket::Kind::QueryHeap:
		occlusionQuery_createHeap_record();
		break;
	case BackendPacket::Kind::BindVertices: {
		const auto bytes = arena.Read(packet.bytes);
		buffer_bindVertexBuffers_record({reinterpret_cast<const BindBufferParam *>(bytes.data()), bytes.size() / sizeof(BindBufferParam)});
		break;
	}
	case BackendPacket::Kind::BindUniform:
		buffer_bindUniformBuffer_record(packet.words[0], packet.words[1], packet.words[2], packet.words[3]);
		break;
	case BackendPacket::Kind::BindFramebuffer: {
		auto *fbo = static_cast<CachedFBOD3D12 *>(m_resources.Resolve(packet.resources[0]));
		if (m_state.activeFBO != fbo)
		{
			m_state.activeFBO = fbo;
			m_state.renderTargetsDirty = true;
		}
		break;
	}
	case BackendPacket::Kind::Viewport: {
		D3D12_VIEWPORT viewport;
		memcpy(&viewport, packet.words, sizeof(viewport));
		renderTarget_setViewport_record(viewport);
		break;
	}
	case BackendPacket::Kind::Scissor: {
		D3D12_RECT scissor;
		memcpy(&scissor, packet.words, sizeof(scissor));
		renderTarget_setScissor_record(scissor);
		break;
	}
	case BackendPacket::Kind::TextureCreate:
		m_resources.Get<LatteTextureD3D12>(packet.resources[0])->CreateNative();
		break;
	case BackendPacket::Kind::FramebufferCreate:
		m_resources.Get<CachedFBOD3D12>(packet.resources[0])->CreateNative();
		break;
	case BackendPacket::Kind::TextureRelease:
		m_resources.Get<LatteTextureD3D12>(packet.resources[0])->ReleaseNative();
		break;
	case BackendPacket::Kind::ViewRelease:
		m_resources.Get<LatteTextureViewD3D12>(packet.resources[0])->ReleaseNative();
		break;
	case BackendPacket::Kind::FramebufferRelease:
		m_resources.Get<CachedFBOD3D12>(packet.resources[0])->ReleaseNative();
		break;
	case BackendPacket::Kind::SurfaceCopy:
		surfaceCopy_record(packet);
		break;
	case BackendPacket::Kind::BufferUpload: {
		const auto bytes = arena.Read(packet.bytes);
		bufferCache_upload_record(bytes.data(), static_cast<uint32>(bytes.size()), packet.words[0]);
		break;
	}
	case BackendPacket::Kind::IndexCreate: {
		auto *index = m_resources.Get<IndexResource>(packet.resources[0]);
		index->native = m_indexAllocator->Allocate(packet.words[0], 4);
		if (!index->native)
			throw std::runtime_error("Latte native index allocation failed");
		break;
	}
	case BackendPacket::Kind::IndexUpload: {
		auto *index = m_resources.Get<IndexResource>(packet.resources[0]);
		const auto bytes = arena.Read(packet.bytes);
		memcpy(index->native->cpuPtr + packet.words[0], bytes.data(), bytes.size());
		LatteProfiler::Add(LatteProfiler::Counter::UploadBytes, bytes.size());
		LatteProfiler::Add(LatteProfiler::Counter::IndexUploadBytes, bytes.size());
		break;
	}
	case BackendPacket::Kind::IndexRelease: {
		auto *index = m_resources.Get<IndexResource>(packet.resources[0]);
		m_indexAllocator->Free(index->native);
		m_resources.Release(packet.resources[0]);
		delete index;
		break;
	}
	case BackendPacket::Kind::BufferInit:
		bufferCache_init_record(packet.words[0], packet.words[1]);
		break;
	case BackendPacket::Kind::NativeRelease:
		ReleaseObjectDeferred(std::move(*m_resources.Get<ComPtr<IUnknown>>(packet.resources[0])));
		InvalidateDrawState();
		break;
	case BackendPacket::Kind::DebugSnapshot: {
		auto &words = m_operations.Result().words;
		words = {m_samplerCache->GetCount(), m_gpuSamplerHeap->GetUsedCount(), m_uploadRing->GetUsedBytes() / 1024,
				 m_deferredReleaser.GetCount(), m_statDrawsPerFrame, m_statSubmitsPerFrame, m_statBarriersPerFrame, m_uploadRing->GetSize() / 1024};
		break;
	}
	case BackendPacket::Kind::TexturePlane:
		texture_recordPlane(packet, arena);
		break;
	}
	if (m_pipeline.Threaded() && m_reportFrames)
	{
		LatteProfiler::Checkpoint(LatteProfiler::Clock::now());
		auto &result = m_operations.Result();
		result.kind = ProfileResult;
		result.words[0] = m_reportFrames;
		result.words[1] = std::bit_cast<uint64>(m_reportSeconds);
		result.bytes.resize(sizeof(LatteProfiler::State));
		memcpy(result.bytes.data(), &LatteProfiler::s_state, sizeof(LatteProfiler::State));
		LatteProfiler::Reset(LatteProfiler::Clock::now());
		m_reportFrames = 0;
	}
	if (packet.kind != BackendPacket::Kind::UIRelease && packet.kind != BackendPacket::Kind::RegistryRetire && packet.kind != BackendPacket::Kind::IndexRelease)
		for (auto id : packet.resources)
			m_resources.Use(id, m_operations.ActiveHeader().sequence, GetRecordingSubmissionId());
}

void D3D12Renderer::ApplyFrontCompletions()
{
	m_pipeline.CheckFront();
	if (m_applyingCompletions)
		return;
	m_applyingCompletions = true;
	try
	{
		if (m_pipeline.Threaded())
			m_operations.Poll();
		const auto completed = m_pipeline.Threaded() ? m_operations.CompletedFence() : m_completedFenceValue;
		if (completed != m_frontCompleted || m_frontReadbacksReady)
		{
			m_frontCompleted = completed;
			m_frontReadbacksReady = false;
			LatteTextureReadback_UpdateFinishedTransfers(false);
		}
	}
	catch (...)
	{
		m_applyingCompletions = false;
		throw;
	}
	m_applyingCompletions = false;
}

void D3D12Renderer::WorkerIdle()
{
	m_pipeline.CheckBack();
	ServicePendingPresent(false);
	ProcessPadSwapChainShutdown();
	NotifyLatteCommandProcessorIdle_record();
	ProcessFinishedSubmissions();
}

void D3D12Renderer::texture_preparePlane(LatteTextureD3D12 *texture, uint32 mip, uint32 slice, uint32 plane, const uint8 *data, uint32 rowBytes, uint32 rows, uint32 width, uint32 blockDim, bool first)
{
	m_pipeline.CheckFront();
	if (!rowBytes || !rows)
		return;
	const auto capacity = m_operations.Staging().Capacity();
	if (rowBytes > capacity)
		throw std::runtime_error("Texture row exceeds Latte staging capacity");
	const uint32 rowsPerPacket = static_cast<uint32>(capacity / rowBytes);
	for (uint32 row = 0; row < rows;)
	{
		const uint32 chunkRows = std::min(rows - row, rowsPerPacket);
		BackendPacket packet{};
		packet.kind = BackendPacket::Kind::TexturePlane;
		packet.resources[0] = texture->recordingId;
		packet.words[0] = mip;
		packet.words[1] = slice;
		packet.words[2] = plane;
		packet.words[3] = rowBytes;
		packet.words[4] = row;
		packet.words[5] = chunkRows;
		packet.words[6] = first && row == 0;
		packet.words[7] = width;
		packet.words[8] = blockDim;
		packet.bytes = StageBytes(data + static_cast<size_t>(row) * rowBytes, static_cast<size_t>(chunkRows) * rowBytes);
		ExecuteBackend(LattePipeline::Opcode::TextureUpload, packet);
		row += chunkRows;
	}
}

void D3D12Renderer::texture_recordPlane(const BackendPacket &packet, const LattePipeline::Arena &arena)
{
	m_pipeline.CheckBack();
	auto *tex = m_resources.Get<LatteTextureD3D12>(packet.resources[0]);
	const uint32 mip = packet.words[0], slice = packet.words[1], plane = packet.words[2];
	const uint32 srcRowBytes = packet.words[3], firstRow = packet.words[4], rows = packet.words[5];
	const uint32 width = packet.words[7], blockDim = packet.words[8];
	const bool is3D = tex->Is3DTexture();
	if (packet.words[6])
	{
		TransitionTexture(tex, mip, 1, is3D ? 0 : slice, 1, D3D12_RESOURCE_STATE_COPY_DEST);
		FlushBarriers();
	}
	const uint32 sub = tex->GetSubresourceIndex(mip, slice, plane);
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
	UINT numRows;
	UINT64 rowSize, totalBytes;
	m_device->GetCopyableFootprints(&tex->GetDesc(), sub, 1, 0, &layout, &numRows, &rowSize, &totalBytes);
	if (firstRow >= numRows)
		return;
	const uint32 copyRows = std::min(rows, numRows - firstRow);
	const uint32 copyRowBytes = std::min<uint32>(srcRowBytes, static_cast<uint32>(rowSize));
	const uint32 rowPitch = AlignUp<uint32>(copyRowBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
	const auto source = arena.Read(packet.bytes);
	auto upload = AllocateUpload(static_cast<uint64>(rowPitch) * copyRows, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
	for (uint32 row = 0; row < copyRows; ++row)
		memcpy(upload.cpuPtr + static_cast<size_t>(row) * rowPitch, source.data() + static_cast<size_t>(row) * srcRowBytes, copyRowBytes);
	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = tex->GetResource();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.SubresourceIndex = sub;
	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = upload.resource;
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint.Offset = upload.offset;
	src.PlacedFootprint.Footprint.Format = layout.Footprint.Format;
	src.PlacedFootprint.Footprint.Width = std::min<uint32>(layout.Footprint.Width, AlignUp<uint32>(width, blockDim));
	src.PlacedFootprint.Footprint.Height = std::min<uint32>(layout.Footprint.Height - firstRow * blockDim, copyRows * blockDim);
	src.PlacedFootprint.Footprint.Depth = 1;
	src.PlacedFootprint.Footprint.RowPitch = rowPitch;
	m_cmdList->CopyTextureRegion(&dst, 0, firstRow * blockDim, is3D ? slice : 0, &src, nullptr);
	LatteProfiler::Add(LatteProfiler::Counter::TextureBytes, static_cast<uint64>(copyRowBytes) * copyRows);
	m_hasRecordedWork = true;
}

void D3D12Renderer::InitializeTexture(LattePipeline::ResourceId id)
{
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::TextureCreate;
	packet.resources[0] = id;
	ExecuteBackend(LattePipeline::Opcode::Create, packet);
}

void D3D12Renderer::InitializeFramebuffer(LattePipeline::ResourceId id)
{
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::FramebufferCreate;
	packet.resources[0] = id;
	ExecuteBackend(LattePipeline::Opcode::Create, packet);
}

void D3D12Renderer::ReleaseTexture(LattePipeline::ResourceId id)
{
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::TextureRelease;
	packet.resources[0] = id;
	ExecuteBackend(LattePipeline::Opcode::Release, packet);
}

void D3D12Renderer::ReleaseView(LattePipeline::ResourceId id)
{
	for (auto &view : m_frontState.boundTexture)
		if (view && view->recordingId == id)
			view = nullptr;
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::ViewRelease;
	packet.resources[0] = id;
	ExecuteBackend(LattePipeline::Opcode::Release, packet);
}

void D3D12Renderer::ReleaseFramebuffer(LattePipeline::ResourceId id)
{
	if (m_frontState.activeFBO && m_frontState.activeFBO->recordingId == id)
		m_frontState.activeFBO = nullptr;
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::FramebufferRelease;
	packet.resources[0] = id;
	ExecuteBackend(LattePipeline::Opcode::Release, packet);
}

void D3D12Renderer::renderTarget_setViewport_record(D3D12_VIEWPORT viewport)
{
	if (memcmp(&viewport, &m_state.viewport, sizeof(viewport)) == 0)
		return;
	m_state.viewport = viewport;
	m_state.viewportDirty = true;
}
void D3D12Renderer::renderTarget_setScissor_record(D3D12_RECT scissor)
{
	if (memcmp(&scissor, &m_state.scissor, sizeof(scissor)) == 0)
		return;
	m_state.scissor = scissor;
	m_state.scissorDirty = true;
}

bool D3D12Renderer::BeginFrame_record(bool mainWindow)
{
	if (!AcquireBackbuffer(mainWindow))
		return false;
	ClearColorbuffer(!mainWindow);
	return true;
}
void D3D12Renderer::capture_record(const BackendPacket &packet)
{
	auto *tex = m_resources.Get<LatteTextureD3D12>(packet.resources[0]);
	const uint32 slice = packet.words[0];
	const sint32 width = packet.words[1], height = packet.words[2];
	const uint32 rowPitch = AlignUp<uint32>((uint32)width * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
	const uint64 size = (uint64)rowPitch * height;
	const uint64 offset = m_readbackRing->Allocate(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

	TransitionTexture(tex, 0, 1, slice, 1, D3D12_RESOURCE_STATE_COPY_SOURCE);
	FlushBarriers();
	D3D12_TEXTURE_COPY_LOCATION s{}, d{};
	s.pResource = tex->GetResource();
	s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	s.SubresourceIndex = tex->GetSubresourceIndex(0, slice);
	d.pResource = m_readbackRing->GetResource();
	d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	d.PlacedFootprint.Offset = offset;
	d.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_TYPELESS, (UINT)width, (UINT)height, 1, rowPitch};
	D3D12_BOX box{0, 0, 0, (UINT)width, (UINT)height, 1};
	m_cmdList->CopyTextureRegion(&d, 0, 0, 0, &s, &box);
	SubmitCommandList(true);

	auto &bytes = m_operations.Result().bytes;
	bytes.resize(size);
	memcpy(bytes.data(), m_readbackRing->GetCPUPtr(offset), size);
}

void *D3D12Renderer::CreateUITexture(const uint8 *rgba, sint32 width, sint32 height)
{
	m_pipeline.CheckFront();
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::UICreate;
	packet.words[0] = width;
	packet.words[1] = height;
	const auto reply = ExecuteBackend(LattePipeline::Opcode::Create, packet);
	void *texture = reinterpret_cast<void *>(reply.words[0]);
	if (!texture || width <= 0 || height <= 0)
		return texture;
	const uint32 rowBytes = static_cast<uint32>(width) * 4;
	const uint32 rowsPerPacket = static_cast<uint32>(m_operations.Staging().Capacity() / rowBytes);
	if (!rowsPerPacket)
		throw std::runtime_error("UI texture row exceeds Latte staging capacity");
	for (uint32 row = 0; row < static_cast<uint32>(height);)
	{
		const uint32 rows = std::min<uint32>(static_cast<uint32>(height) - row, rowsPerPacket);
		BackendPacket upload{};
		upload.kind = BackendPacket::Kind::UIUpload;
		upload.resources[0] = LattePipeline::FromOpaqueId(texture);
		upload.words[0] = width;
		upload.words[1] = row;
		upload.words[2] = rows;
		upload.words[3] = row + rows == static_cast<uint32>(height);
		upload.bytes = StageBytes(rgba + static_cast<size_t>(row) * rowBytes, static_cast<size_t>(rows) * rowBytes);
		ExecuteBackend(LattePipeline::Opcode::TextureUpload, upload);
		row += rows;
	}
	return texture;
}
void D3D12Renderer::ui_record(const BackendPacket &packet)
{
	auto *snapshot = m_resources.Get<UISnapshot>(packet.resources[0]);
	auto *swapChain = GetSwapChainObj(packet.words[0] != 0);
	if (!swapChain || !swapChain->IsValid())
		return;
	m_imgui->Render(&snapshot->data, swapChain->GetCurrentRTV(), D3D12SwapChain::kFormat, swapChain->GetWidth(), swapChain->GetHeight());
	swapChain->SetDefinedContent();
}

static uint64 CurrentThreadCpuNs()
{
	FILETIME created, exited, kernel, user;
	if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
		return 0;
	return (((uint64(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
			((uint64(user.dwHighDateTime) << 32) | user.dwLowDateTime)) *
		   100;
}

void D3D12Renderer::AcceptCompletion(const LattePipeline::Completion &completion)
{
	m_pipeline.CheckFront();
	m_resources.Recycle(completion.retiredResources);
	if (completion.kind == QueryResult)
	{
		AcceptQueryResult(completion);
		return;
	}
	if (completion.kind == QueryBatchResult)
	{
		AcceptQueryBatch(completion);
		return;
	}
	if (completion.kind == ReadbackResult)
	{
		AcceptReadbackResult(completion);
		return;
	}
	if (completion.kind != ProfileResult)
		return;
	LatteProfiler::State back;
	if (completion.bytes.size() != sizeof(back))
		throw std::logic_error("Invalid Latte profile reply");
	memcpy(&back, completion.bytes.data(), sizeof(back));
	auto &front = LatteProfiler::s_state;
	// Counters from both actors describe the same command stream. Wall-time
	// slots/covered remain exclusively front; the back collector is separate.
	for (size_t i = 0; i < front.counters.size(); ++i)
		front.counters[i] += back.counters[i];
	const auto now = LatteProfiler::Clock::now();
	const auto timing = m_operations.Metrics();
	const auto stall = timing.backpressureNs - m_profilePipelineTiming.backpressureNs;
	const auto reply = timing.replyWaitNs - m_profilePipelineTiming.replyWaitNs;
	// OS thread CPU time excludes waits and preemption without subtracting
	// overlapping PM4-idle/reply intervals (OSScreen can present inside idle).
	front.backRecordWallNs = completion.backBusyNs - m_profilePipelineTiming.backBusyNs;
	front.frontStallNs = stall;
	front.frontReplyNs = reply;
	front.backIdleNs = completion.backIdleNs - m_profilePipelineTiming.backIdleNs;
	front.ringHighWater = timing.highWater;
	const auto cpu = CurrentThreadCpuNs();
	front.frontCpuNs = cpu >= m_frontCpuStart ? cpu - m_frontCpuStart : 0;
	front.backCpuNs = back.backCpuNs;
	front.frontBusyNs = front.frontCpuNs;
	front.backBusyNs = front.backCpuNs;
	front.threaded = true;
	front.operationSequence = m_pipeline.Sequence();
	front.sourceFrame = m_pipeline.Frame();
	front.recordedSequence = completion.tickets.recorded;
	front.submittedSequence = completion.tickets.submitted;
	front.completedSequence = completion.tickets.completed;
	LatteProfiler::Report(now, std::bit_cast<double>(completion.words[1]), completion.words[0]);
	m_profilePipelineTiming = timing;
	m_profilePipelineTiming.backBusyNs = completion.backBusyNs;
	m_profilePipelineTiming.backIdleNs = completion.backIdleNs;
	m_frontCpuStart = cpu;
}

void D3D12Renderer::UpdatePipelineProfile()
{
	if (!LatteProfiler::s_state.enabled || (m_pipeline.Threaded() && m_operations.Recording()))
		return;
	auto &profile = LatteProfiler::s_state;
	profile.operationSequence = m_operations.Recording() ? m_operations.ActiveHeader().sequence : m_pipeline.Sequence();
	profile.sourceFrame = m_operations.Recording() ? m_operations.ActiveHeader().frame : m_pipeline.Frame();
	const auto &tickets = m_operations.Progress();
	profile.recordedSequence = tickets.recorded;
	profile.submittedSequence = tickets.submitted;
	profile.completedSequence = tickets.completed;
	if (!m_pipeline.Threaded())
		profile.ringHighWater = m_operations.Metrics().highWater;
}

void D3D12Renderer::RetireResource(LattePipeline::ResourceId id)
{
	if (m_operations.Recording())
	{
		m_resources.Release(id);
		return;
	}
	m_pipeline.CheckFront();
	if (!id)
		return;
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::RegistryRetire;
	packet.resources[0] = id;
	ExecuteBackend(LattePipeline::Opcode::Release, packet);
}

#endif // ENABLE_D3D12
