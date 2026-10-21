// SPDX-License-Identifier: MIT
#ifdef ENABLE_D3D12
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Ffx.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12SwapChain.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h"
#include "../../../../../../src/fsr3_policy.h"
#include "../../../../../../src/fsr3_history.h"
#include "../../../../../../src/log.h"
#ifdef BOTW_HAS_FFX
#include <ffx_api.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>
#include <ffx_framegeneration.h>
#include <dx12/ffx_api_framegeneration_dx12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#endif

struct D3D12Ffx::Impl
{
	D3D12Renderer &renderer;
	explicit Impl(D3D12Renderer &r) : renderer(r)
	{
	}
#ifdef BOTW_HAS_FFX
	HMODULE loader = nullptr;
	PfnFfxCreateContext create = nullptr;
	PfnFfxDestroyContext destroy = nullptr;
	PfnFfxConfigure configure = nullptr;
	PfnFfxQuery query = nullptr;
	PfnFfxDispatch dispatch = nullptr;
	ffxContext upscale = nullptr, generation = nullptr, swap = nullptr;
	ffxCreateBackendDX12Desc backend{};
	ffxOverrideVersion upscaleVersion{}, generationVersion{};
	ffxCreateContextDescFrameGenerationVersion generationApi{};
	ffxCreateContextDescUpscale upscaleDesc{};
	ffxCreateContextDescFrameGeneration generationDesc{};
	ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrapDesc{};
	ComPtr<IDXGISwapChain4> wrapped;
	D3D12SwapChain *chain = nullptr;
	ComPtr<ID3D12Resource> motion, mask, normalizedDepth, linearColor, linearOutput;
	ComPtr<ID3D12PipelineState> linearPipeline, displayPipeline;
	D3D12_CPU_DESCRIPTOR_HANDLE linearRTV{}, linearOutputRTV{};
	D3D12_RESOURCE_STATES linearState = D3D12_RESOURCE_STATE_COMMON, linearOutputState = D3D12_RESOURCE_STATE_COMMON;
	ComPtr<ID3D12RootSignature> depthRoot;
	ComPtr<ID3D12PipelineState> depthPipeline;
	D3D12_CPU_DESCRIPTOR_HANDLE depthRTV{}, depthSRV{};
	D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COMMON;
	struct Probe
	{
		ComPtr<ID3D12Resource> resource;
		uint64_t submission = 0, sequence = 0;
		double frameMs = 0;
	};
	std::array<Probe, 3> probes{};
	uint64_t probeSequence = 0;
	fsr3::FrameGenerationPolicy fgPolicy;
	fsr3::History history;
	bool resetHistory = true;
	D3D12_CPU_DESCRIPTOR_HANDLE motionRTV{}, maskRTV{};
	D3D12_RESOURCE_STATES motionState = D3D12_RESOURCE_STATE_COMMON, maskState = D3D12_RESOURCE_STATE_COMMON;
	uint32 iw = 0, ih = 0, ow = 0, oh = 0;
	bool failed = false, fgFailed = false, warned = false, fgWarned = false, missingDepthLogged = false;
	bool initialized = false, prepared = false, generating = false;
	std::atomic<bool> callbackFailed{false};
	std::array<std::atomic<bool>, 16> frameResets{};
	uint64_t frame = 0;
	double realMs = 1000.0 / 60.0;
	std::chrono::steady_clock::time_point previous{};

	void Fail(const char *reason, bool fgOnly = false)
	{
		if (fgOnly)
		{
			if (!fgWarned)
				LOG("FidelityFX: %s; frame generation disabled, keeping FSR 3.1 upscaling", reason);
			fgWarned = fgFailed = true;
			return;
		}
		if (!warned)
			LOG("FidelityFX: %s; using FSR 1 (then output scaling if unavailable)", reason);
		warned = failed = true;
	}
	bool ResetGeneration() noexcept
	try
	{
		if (generation)
		{
			ffxConfigureDescFrameGeneration config{};
			config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
			config.swapChain = wrapped.Get();
			if (configure(&generation, &config.header) != FFX_API_RETURN_OK)
				Fail("FG disable failed", true);
		}
		if (swap)
		{
			ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{};
			wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
			if (dispatch(&swap, &wait.header) != FFX_API_RETURN_OK)
				Fail("FG present drain failed", true);
		}
		if (generation || swap)
			renderer.Flush(true);
		if (generation && destroy(&generation, nullptr) != FFX_API_RETURN_OK)
			Fail("FG context destruction failed", true);
		// The SDK consumes the original chain when wrapping; it cannot be restored.
		// A disabled interpolation chain remains a normal presentable IDXGISwapChain.
		// Keep that chain alive while dropping the SDK context's independent reference.
		if (swap && destroy(&swap, nullptr) != FFX_API_RETURN_OK)
			Fail("swapchain context destruction failed", true);
		generation = swap = nullptr;
		if (chain)
		{
			chain->SetFrameGeneration(false);
			if (!chain->IsValid() && !chain->ReplaceNative(wrapped.Get()))
			{
				Fail("swapchain recovery failed; recreating native swapchain", true);
				if (!chain->Recreate())
				{
					Fail("native swapchain recreation failed");
					// Recreate preserves any presentable chain. Retry the returned chain
					// if its buffer views were temporarily unavailable during wrapping.
					chain->ReplaceNative(wrapped.Get());
				}
			}
		}
		wrapped.Reset();
		chain = nullptr;
		prepared = generating = false;
		callbackFailed.store(false);
		return true;
	}

	catch (...)
	{
		Fail("FG teardown failed");
		generating = prepared = false;
		if (chain)
			chain->SetFrameGeneration(false);
		return false;
	}

	bool Version(uint64_t type, ffxOverrideVersion &out)
	{
		uint64_t count = 0;
		ffxQueryDescGetVersions versions{};
		versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
		versions.createDescType = type;
		versions.device = renderer.GetDevice();
		versions.outputCount = &count;
		if (query(nullptr, &versions.header) != FFX_API_RETURN_OK || !count || count > 128)
			return false;
		std::vector<uint64_t> ids(count);
		std::vector<const char *> names(count);
		versions.versionIds = ids.data();
		versions.versionNames = names.data();
		if (query(nullptr, &versions.header) != FFX_API_RETURN_OK || count > ids.size())
			return false;
		for (size_t i = 0; i < count; ++i)
		{
			// Pin the FSR 3.1 provider: SDK 2.3 also contains newer upscalers.
			const std::string_view name = names[i] ? names[i] : "";
			if (name.find("3.1.") != std::string_view::npos)
			{
				out.header = {FFX_API_DESC_TYPE_OVERRIDE_VERSION, &backend.header};
				out.versionId = ids[i];
				return true;
			}
		}
		return false;
	}
	static ffxReturnCode_t Generate(ffxDispatchDescFrameGeneration *desc, void *user)
	{
		auto &self = *static_cast<Impl *>(user);
		desc->reset = self.frameResets[desc->frameID % self.frameResets.size()].load();
		const auto result = self.dispatch(&self.generation, &desc->header);
		if (result != FFX_API_RETURN_OK)
			self.callbackFailed.store(true);
		return result;
	}
	void CreateDepthPipeline()
	{
		if (depthPipeline && linearPipeline && displayPipeline)
			return;
		const char *source = R"hlsl(
Texture2D<float4> Source : register(t0);
float4 VS(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2,-2) + float2(-1,1), 0, 1);
}
float PS(float4 position : SV_Position) : SV_Target {
    return Source.Load(int3(int2(position.xy), 0)).r;
}
float4 Linear(float4 position : SV_Position) : SV_Target {
    float3 color = Source.Load(int3(int2(position.xy), 0)).rgb;
    color = lerp(color / 12.92, pow((color + 0.055) / 1.055, 2.4), step(0.04045, color));
    return float4(color, 1);
}
float4 Display(float4 position : SV_Position) : SV_Target {
    float3 color = max(0, Source.Load(int3(int2(position.xy), 0)).rgb);
    color = lerp(color * 12.92, 1.055 * pow(color, 1.0 / 2.4) - 0.055, step(0.0031308, color));
    return float4(color, 1);
}
)hlsl";
		auto compile = [&](const char *entry, const char *profile) {
			ComPtr<ID3DBlob> code, errors;
			D3D12_ThrowIfFailed(D3DCompile(source, strlen(source), "FFX_depth", nullptr, nullptr, entry, profile,
										   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors),
								"FFX depth shader");
			return code;
		};
		D3D12_DESCRIPTOR_RANGE range{};
		range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		range.NumDescriptors = 1;
		range.OffsetInDescriptorsFromTableStart = D3D12Const::kStageTableOffsetSRV;
		D3D12_ROOT_PARAMETER parameter{};
		parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameter.DescriptorTable = {1, &range};
		D3D12_ROOT_SIGNATURE_DESC signature{};
		signature.NumParameters = 1;
		signature.pParameters = &parameter;
		ComPtr<ID3DBlob> blob, errors;
		D3D12_ThrowIfFailed(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "FFX depth signature");
		auto *device = renderer.GetDevice();
		D3D12_ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&depthRoot)), "FFX depth root");
		const auto vs = compile("VS", "vs_5_0"), ps = compile("PS", "ps_5_0");
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
		pipeline.pRootSignature = depthRoot.Get();
		pipeline.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
		pipeline.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
		for (auto &target : pipeline.BlendState.RenderTarget)
		{
			target.SrcBlend = target.SrcBlendAlpha = D3D12_BLEND_ONE;
			target.DestBlend = target.DestBlendAlpha = D3D12_BLEND_ZERO;
			target.BlendOp = target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			target.LogicOp = D3D12_LOGIC_OP_NOOP;
		}
		pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED;
		pipeline.SampleMask = UINT_MAX;
		pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pipeline.RasterizerState.DepthClipEnable = TRUE;
		pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		auto &stencil = pipeline.DepthStencilState.FrontFace;
		stencil.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		stencil.StencilFailOp = stencil.StencilDepthFailOp = stencil.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		pipeline.DepthStencilState.BackFace = stencil;
		pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pipeline.NumRenderTargets = 1;
		pipeline.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
		pipeline.SampleDesc.Count = 1;
		D3D12_ThrowIfFailed(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&depthPipeline)), "FFX depth pipeline");
		const auto linear = compile("Linear", "ps_5_0"), display = compile("Display", "ps_5_0");
		pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		pipeline.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
		pipeline.PS = {linear->GetBufferPointer(), linear->GetBufferSize()};
		D3D12_ThrowIfFailed(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&linearPipeline)), "FFX linear pipeline");
		pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		pipeline.PS = {display->GetBufferPointer(), display->GetBufferSize()};
		D3D12_ThrowIfFailed(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&displayPipeline)), "FFX display pipeline");
	}
	void ConvertColor(ID3D12Resource *source, D3D12_RESOURCE_STATES &sourceState,
					  ID3D12Resource *target, D3D12_RESOURCE_STATES &targetState, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
					  ID3D12PipelineState *pipeline, uint32 width, uint32 height)
	{
		renderer.TransitionResource(source, sourceState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		renderer.TransitionResource(target, targetState, D3D12_RESOURCE_STATE_RENDER_TARGET);
		renderer.FlushBarriers();
		renderer.GetDevice()->CreateShaderResourceView(source, nullptr, depthSRV);
		renderer.BindRootSignatureAndHeaps();
		auto *cmd = renderer.GetCommandList();
		cmd->SetGraphicsRootSignature(depthRoot.Get());
		D3D12StageTableDesc table;
		table.srv[0] = depthSRV;
		cmd->SetGraphicsRootDescriptorTable(0, renderer.WriteStageTable(table));
		cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
		const D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
		const D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
		cmd->RSSetViewports(1, &viewport);
		cmd->RSSetScissorRects(1, &scissor);
		cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		cmd->SetPipelineState(pipeline);
		cmd->DrawInstanced(3, 1, 0, 0);
		renderer.InvalidateDrawState();
	}
	FfxApiResource NormalizeDepth(LatteTextureD3D12 *depth)
	{
		renderer.TransitionTexture(depth, 0, 1, 0, 1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		renderer.TransitionResource(normalizedDepth.Get(), depthState, D3D12_RESOURCE_STATE_RENDER_TARGET);
		renderer.FlushBarriers();
		D3D12_SHADER_RESOURCE_VIEW_DESC view{};
		view.Format = depth->GetFormatInfo().srvFormat;
		view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		view.Texture2D.MipLevels = 1;
		renderer.GetDevice()->CreateShaderResourceView(depth->GetResource(), &view, depthSRV);
		renderer.BindRootSignatureAndHeaps();
		auto *cmd = renderer.GetCommandList();
		cmd->SetGraphicsRootSignature(depthRoot.Get());
		D3D12StageTableDesc table;
		table.srv[0] = depthSRV;
		cmd->SetGraphicsRootDescriptorTable(0, renderer.WriteStageTable(table));
		cmd->OMSetRenderTargets(1, &depthRTV, FALSE, nullptr);
		const D3D12_VIEWPORT viewport{0, 0, float(iw), float(ih), 0, 1};
		const D3D12_RECT scissor{0, 0, LONG(iw), LONG(ih)};
		cmd->RSSetViewports(1, &viewport);
		cmd->RSSetScissorRects(1, &scissor);
		cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		cmd->SetPipelineState(depthPipeline.Get());
		cmd->DrawInstanced(3, 1, 0, 0);
		renderer.TransitionResource(normalizedDepth.Get(), depthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		renderer.FlushBarriers();
		renderer.InvalidateDrawState();
		return ffxApiGetResourceDX12(normalizedDepth.Get());
	}
	bool DetectCut(ID3D12Resource *color, D3D12_RESOURCE_STATES &state)
	{
		++probeSequence;
		bool cut = realMs > 100.0 || probeSequence == 1;
		const auto completed = renderer.GetCompletedSubmissionId();
		// Read only completed copies, oldest first, normally one or two frames late.
		for (;;)
		{
			Probe *ready = nullptr;
			for (auto &slot : probes)
				if (slot.submission && slot.submission <= completed && slot.sequence < probeSequence &&
					(!ready || slot.sequence < ready->sequence))
					ready = &slot;
			if (!ready)
				break;
			const D3D12_RANGE read{0, fsr3::History::samples * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT};
			void *data = nullptr;
			if (SUCCEEDED(ready->resource->Map(0, &read, &data)))
			{
				fsr3::History::Image image{};
				for (uint32_t i = 0; i < fsr3::History::samples; ++i)
					for (uint32_t c = 0; c < 3; ++c)
						image[i][c] = static_cast<const uint8_t *>(data)[i * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT + c];
				const D3D12_RANGE written{0, 0};
				ready->resource->Unmap(0, &written);
				cut = history.update(image, ready->frameMs) || cut;
			}
			else
				cut = true;
			ready->submission = 0;
		}
		Probe *destination = nullptr;
		for (auto &slot : probes)
			if (!slot.submission)
			{
				destination = &slot;
				break;
			}
		// A busy GPU can fill the ring: skip this sample rather than wait or overwrite it.
		if (!destination)
			return cut;
		renderer.TransitionResource(color, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
		renderer.FlushBarriers();
		auto *cmd = renderer.GetCommandList();
		D3D12_TEXTURE_COPY_LOCATION src{};
		src.pResource = color;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		for (uint32_t y = 0; y < fsr3::History::rows; ++y)
		{
			for (uint32_t x = 0; x < fsr3::History::columns; ++x)
			{
				const uint32_t index = y * fsr3::History::columns + x;
				const uint32_t px = std::min(iw - 1, (2 * x + 1) * iw / (2 * fsr3::History::columns));
				const uint32_t py = std::min(ih - 1, (2 * y + 1) * ih / (2 * fsr3::History::rows));
				D3D12_TEXTURE_COPY_LOCATION dst{};
				dst.pResource = destination->resource.Get();
				dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				dst.PlacedFootprint.Offset = index * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
				dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
				const D3D12_BOX box{px, py, 0, px + 1, py + 1, 1};
				cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
			}
		}
		destination->submission = renderer.GetRecordingSubmissionId();
		destination->sequence = probeSequence;
		destination->frameMs = realMs;
		return cut;
	}

	double ModeHz() const
	{
		if (!chain)
			return 0;
		if (const double hdmi = fsr3::display_hz.load(); hdmi > 0)
			return hdmi;
		ComPtr<IDXGIOutput> output;
		DXGI_OUTPUT_DESC desc{};
		DEVMODEW mode{};
		mode.dmSize = sizeof(mode);
		// Do not infer 120 Hz from measured Present throughput or a guessed default.
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
		if (SUCCEEDED(chain->GetSwapChain()->GetContainingOutput(&output)) &&
			SUCCEEDED(output->GetDesc(&desc)) && EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode))
			return mode.dmDisplayFrequency;
#endif
		DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen{};
		if (SUCCEEDED(chain->GetSwapChain()->GetFullscreenDesc(&fullscreen)) && fullscreen.RefreshRate.Denominator)
			return double(fullscreen.RefreshRate.Numerator) / fullscreen.RefreshRate.Denominator;
		// CoreWindow may not report a mode; unknown refresh disables FG safely.
		return 0;
	}
#endif
};

D3D12Ffx::D3D12Ffx(D3D12Renderer &renderer) : impl(std::make_unique<Impl>(renderer))
{
#ifdef BOTW_HAS_FFX
	auto &s = *impl;
	const char *overrideValue = std::getenv("BOTW_UPSCALER");
	if (fsr3::select(fsr3::console.load(), false, overrideValue ? overrideValue : "", true, true).path != fsr3::Path::Fsr31)
		return;
	s.loader = LoadLibraryExW(L"amd_fidelityfx_loader_dx12.dll", nullptr,
							  LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!s.loader)
	{
		s.Fail("optional loader/dependency missing");
		return;
	}
	s.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(s.loader, "ffxCreateContext"));
	s.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(s.loader, "ffxDestroyContext"));
	s.configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(s.loader, "ffxConfigure"));
	s.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(s.loader, "ffxQuery"));
	s.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(s.loader, "ffxDispatch"));
	if (!s.create || !s.destroy || !s.configure || !s.query || !s.dispatch)
		s.Fail("loader entry point missing");
	s.backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
	s.backend.device = renderer.GetDevice();
#endif
}
D3D12Ffx::~D3D12Ffx()
{
	Reset();
#ifdef BOTW_HAS_FFX
	const auto loader = impl->loader;
	// Release any retained SDK COM objects before unloading their implementation.
	impl.reset();
	if (loader)
		FreeLibrary(loader);
#endif
}
void D3D12Ffx::Reset() noexcept
try
{
#ifdef BOTW_HAS_FFX
	auto &s = *impl;
	if (!s.upscale && !s.generation && !s.swap && !s.motion && !s.mask)
		return;
	if (!s.ResetGeneration())
		return;
	s.renderer.Flush(true);
	if (s.upscale && s.destroy(&s.upscale, nullptr) != FFX_API_RETURN_OK)
		s.Fail("upscale context destruction failed");
	s.upscale = nullptr;
	s.history.clear();
	for (auto &slot : s.probes)
		slot = {};
	s.probeSequence = 0;
	s.normalizedDepth.Reset();
	s.linearColor.Reset();
	s.linearOutput.Reset();
	if (s.linearRTV.ptr)
		s.renderer.GetStagingRTVHeap().Free(s.linearRTV);
	if (s.linearOutputRTV.ptr)
		s.renderer.GetStagingRTVHeap().Free(s.linearOutputRTV);
	s.linearRTV = {};
	s.linearOutputRTV = {};
	if (s.depthRTV.ptr)
		s.renderer.GetStagingRTVHeap().Free(s.depthRTV);
	if (s.depthSRV.ptr)
		s.renderer.GetStagingViewHeap().Free(s.depthSRV);
	s.depthRTV = {};
	s.depthSRV = {};
	s.motion.Reset();
	s.mask.Reset();
	if (s.motionRTV.ptr)
		s.renderer.GetStagingRTVHeap().Free(s.motionRTV);
	if (s.maskRTV.ptr)
		s.renderer.GetStagingRTVHeap().Free(s.maskRTV);
	s.motionRTV = {};
	s.maskRTV = {};
	s.initialized = s.prepared = s.generating = false;
	s.previous = {};
#endif
}
catch (...)
{
#ifdef BOTW_HAS_FFX
	impl->Fail("FFX teardown failed");
#endif
}

void D3D12Ffx::Resize(uint32 iw, uint32 ih, uint32 ow, uint32 oh, D3D12SwapChain *chain)
{
#ifdef BOTW_HAS_FFX
	auto &s = *impl;
	Reset();
	if (!s.loader || s.failed)
		return;
	s.iw = iw;
	s.ih = ih;
	s.ow = ow;
	s.oh = oh;
	try
	{
		if (!s.Version(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, s.upscaleVersion))
		{
			s.Fail("FSR 3.1 provider unavailable");
			return;
		}
		s.upscaleDesc = {};
		s.upscaleDesc.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &s.upscaleVersion.header};
		s.upscaleDesc.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
		s.upscaleDesc.maxRenderSize = {iw, ih};
		s.upscaleDesc.maxUpscaleSize = {ow, oh};
		if (s.create(&s.upscale, &s.upscaleDesc.header, nullptr) != FFX_API_RETURN_OK)
		{
			s.Fail("upscale context creation failed");
			Reset();
			return;
		}
		auto texture = [&](DXGI_FORMAT format, ComPtr<ID3D12Resource> &resource, D3D12_CPU_DESCRIPTOR_HANDLE &rtv,
						   uint32 width = 0, uint32 height = 0) {
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = width ? width : iw;
			desc.Height = height ? height : ih;
			desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
			desc.Format = format;
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
				desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			auto heap = D3D12_HeapProps(D3D12_HEAP_TYPE_DEFAULT);
			D3D12_ThrowIfFailed(s.renderer.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
																				D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)),
								"FFX input texture");
			rtv = s.renderer.GetStagingRTVHeap().Allocate();
			s.renderer.GetDevice()->CreateRenderTargetView(resource.Get(), nullptr, rtv);
		};
		texture(DXGI_FORMAT_R16G16_FLOAT, s.motion, s.motionRTV);
		texture(DXGI_FORMAT_R8_UNORM, s.mask, s.maskRTV);
		texture(DXGI_FORMAT_R32_FLOAT, s.normalizedDepth, s.depthRTV);
		texture(DXGI_FORMAT_R16G16B16A16_FLOAT, s.linearColor, s.linearRTV);
		texture(DXGI_FORMAT_R16G16B16A16_FLOAT, s.linearOutput, s.linearOutputRTV, ow, oh);
		s.linearState = s.linearOutputState = D3D12_RESOURCE_STATE_COMMON;
		s.depthSRV = s.renderer.GetStagingViewHeap().Allocate();
		s.depthState = D3D12_RESOURCE_STATE_COMMON;
		s.CreateDepthPipeline();
		s.motionState = s.maskState = D3D12_RESOURCE_STATE_COMMON;
		auto readbackHeap = D3D12_HeapProps(D3D12_HEAP_TYPE_READBACK);
		D3D12_RESOURCE_DESC buffer{};
		buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		buffer.Width = fsr3::History::samples * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
		buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
		buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		for (auto &slot : s.probes)
			D3D12_ThrowIfFailed(s.renderer.GetDevice()->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE,
																				&buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.resource)),
								"FFX scene probe");
		// No clean guest projection hook: jitter stays zero; querying an offset alone
		// would not jitter scene rendering and would provide incorrect reconstruction data.
		LOG("FidelityFX: FSR 3.1 %ux%u -> %ux%u; zero motion, no jitter, image-cut reset", iw, ih, ow, oh);
		if (!chain || s.fgFailed)
			return;
		if (!s.Version(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, s.generationVersion))
		{
			s.Fail("FG 3.1 provider unavailable", true);
			s.ResetGeneration();
			return;
		}
		s.generationDesc = {};
		s.generationApi = {};
		s.generationApi.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION, &s.generationVersion.header};
		s.generationApi.version = FFX_FRAMEGENERATION_VERSION;
		s.generationDesc.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, &s.generationApi.header};
		s.generationDesc.displaySize = {chain->GetWidth(), chain->GetHeight()};
		s.generationDesc.maxRenderSize = {iw, ih};
		s.generationDesc.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
		if (s.create(&s.generation, &s.generationDesc.header, nullptr) != FFX_API_RETURN_OK)
		{
			s.Fail("FG context creation failed", true);
			s.ResetGeneration();
			return;
		}
		// SDK 2.3.0's wrap implementation requires GetHwnd/GetFullscreenDesc.
		// CoreWindow/composition chains are unsupported: leave them presentable.
		HWND window = nullptr;
		DXGI_SWAP_CHAIN_DESC1 nativeDesc{};
		DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen{};
		if (!chain->IsValid() || FAILED(chain->GetSwapChain()->GetDesc1(&nativeDesc)) ||
			FAILED(chain->GetSwapChain()->GetFullscreenDesc(&fullscreen)) ||
			FAILED(chain->GetSwapChain()->GetHwnd(&window)) || !window || !fullscreen.Windowed ||
			FAILED(chain->GetSwapChain()->QueryInterface(IID_PPV_ARGS(&s.wrapped))))
		{
			s.Fail("swapchain does not support SDK HWND replacement", true);
			s.ResetGeneration();
			return;
		}
		s.chain = chain;
		s.renderer.Flush(true);
		chain->ReleaseNative();
		// ffxCreateContextDescFrameGenerationSwapChainWrapDX12::swapchain is in/out.
		// ffxReplaceSwapchainForFrameinterpolationDX12 releases the input reference
		// before creating its replacement. Drop ALL other chain/buffer references;
		// retaining an original AddRef prevents recreation on the same window.
		// Detach transfers our sole input reference; adopt only the successful output.
		// On failure the input may already be consumed, so never Release that pointer.
		auto *replacement = s.wrapped.Detach();
		s.wrapDesc = {};
		s.wrapDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
		s.wrapDesc.swapchain = &replacement;
		s.wrapDesc.gameQueue = s.renderer.GetQueue();
		if (s.create(&s.swap, &s.wrapDesc.header, nullptr) != FFX_API_RETURN_OK)
		{
			s.Fail("swapchain replacement failed", true);
			s.ResetGeneration();
			return;
		}
		s.wrapped.Attach(replacement);
		if (!chain->ReplaceNative(s.wrapped.Get()))
		{
			s.Fail("wrapped backbuffer acquisition failed", true);
			s.ResetGeneration();
			return;
		}
		// ponytail: BotW HUD is already baked into color; no HUD-less surface or
		// separate UI pass is available. Generated frames may distort HUD/text.
	}
	catch (const std::exception &)
	{
		s.Fail("FFX resource creation failed");
		Reset();
	}
#else
	(void)iw;
	(void)ih;
	(void)ow;
	(void)oh;
	(void)chain;
#endif
}

bool D3D12Ffx::Upscale(ID3D12Resource *color, D3D12_RESOURCE_STATES &colorState,
					   ID3D12Resource *output, D3D12_RESOURCE_STATES &outputState, LatteTextureD3D12 *depth)
{
#ifdef BOTW_HAS_FFX
	auto &s = *impl;
	s.prepared = false;
	if (!s.upscale || s.failed)
		return false;
	// Accept an actual, current-frame full-resolution scene depth target.
	// Never feed a fabricated constant-depth surface to FSR.
	if (!depth || depth->GetDesc().Width != s.iw || depth->GetDesc().Height != s.ih ||
		depth->GetDesc().DepthOrArraySize != 1 || depth->GetDesc().MipLevels != 1 ||
		depth->GetDesc().SampleDesc.Count != 1 ||
		(depth->GetDesc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
		(depth->GetFormatInfo().srvFormat != DXGI_FORMAT_R32_FLOAT &&
		 depth->GetFormatInfo().srvFormat != DXGI_FORMAT_R16_UNORM &&
		 depth->GetFormatInfo().srvFormat != DXGI_FORMAT_R24_UNORM_X8_TYPELESS &&
		 depth->GetFormatInfo().srvFormat != DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS))
	{
		if (!s.missingDepthLogged)
		{
			LOG("FidelityFX: no usable scene depth this frame; using FSR 1");
			s.missingDepthLogged = true;
		}
		s.history.clear();
		return false;
	}
	const auto now = std::chrono::steady_clock::now();
	if (s.previous != std::chrono::steady_clock::time_point{})
		s.realMs = std::clamp(std::chrono::duration<double, std::milli>(now - s.previous).count(), 1.0, 1000.0);
	s.previous = now;
	auto &r = s.renderer;
	s.resetHistory = s.DetectCut(color, colorState);
	if (s.failed)
		return false;
	auto *cmd = r.GetCommandList();
	if (!s.initialized)
	{
		r.TransitionResource(s.motion.Get(), s.motionState, D3D12_RESOURCE_STATE_RENDER_TARGET);
		r.TransitionResource(s.mask.Get(), s.maskState, D3D12_RESOURCE_STATE_RENDER_TARGET);
		r.FlushBarriers();
		const float zero[4] = {}, one[4] = {1, 1, 1, 1};
		cmd->ClearRenderTargetView(s.motionRTV, zero, 0, nullptr);
		cmd->ClearRenderTargetView(s.maskRTV, one, 0, nullptr);
		s.initialized = true;
	}
	// The existing output shader produces display-referred SDR. FSR 3.1 expects
	// linear color; convert explicitly instead of using newer-provider-only flags.
	s.ConvertColor(color, colorState, s.linearColor.Get(), s.linearState, s.linearRTV, s.linearPipeline.Get(), s.iw, s.ih);
	r.TransitionResource(s.linearColor.Get(), s.linearState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	r.TransitionResource(s.linearOutput.Get(), s.linearOutputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	r.TransitionResource(s.motion.Get(), s.motionState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	r.TransitionResource(s.mask.Get(), s.maskState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	r.FlushBarriers();
	const auto depthInput = s.NormalizeDepth(depth);
	ffxDispatchDescUpscale desc{};
	desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
	desc.commandList = cmd;
	desc.color = ffxApiGetResourceDX12(s.linearColor.Get());
	desc.depth = depthInput;
	desc.motionVectors = ffxApiGetResourceDX12(s.motion.Get());
	desc.reactive = desc.transparencyAndComposition = ffxApiGetResourceDX12(s.mask.Get());
	desc.output = ffxApiGetResourceDX12(s.linearOutput.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
	desc.motionVectorScale = {1, 1};
	desc.renderSize = {s.iw, s.ih};
	desc.upscaleSize = {s.ow, s.oh};
	desc.frameTimeDelta = float(s.realMs);
	desc.preExposure = 1;
	// ponytail: Wii U has no motion buffer and the decompiled shaders expose no
	// reliable camera matrices/projection hook. Zero motion + fully reactive masks
	// limit ghosting but lose temporal detail/antialiasing. Image cuts are heuristic:
	// small or spatially similar cuts may be missed, fast pans may reset unnecessarily.
	// Near/far/FOV are nominal, not recovered game-camera data. Do not claim TAA quality.
	desc.reset = s.resetHistory;
	desc.cameraNear = 0.1f;
	desc.cameraFar = 10000;
	desc.cameraFovAngleVertical = 1.04719755f;
	desc.viewSpaceToMetersFactor = 1;
	r.InvalidateDrawState();
	if (s.dispatch(&s.upscale, &desc.header) != FFX_API_RETURN_OK)
	{
		s.Fail("upscale dispatch failed");
		return false;
	}
	r.GetDevice()->CreateRenderTargetView(output, nullptr, s.linearOutputRTV);
	s.ConvertColor(s.linearOutput.Get(), s.linearOutputState, output, outputState,
				   s.linearOutputRTV, s.displayPipeline.Get(), s.ow, s.oh);
	++s.frame;
	s.frameResets[s.frame % s.frameResets.size()].store(s.resetHistory);
	if (s.generation && s.swap && !s.fgFailed)
	{
		ffxDispatchDescFrameGenerationPrepareV2 prepare{};
		prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
		prepare.commandList = cmd;
		prepare.frameID = s.frame;
		prepare.renderSize = desc.renderSize;
		prepare.motionVectorScale = desc.motionVectorScale;
		prepare.depth = desc.depth;
		prepare.motionVectors = desc.motionVectors;
		prepare.frameTimeDelta = desc.frameTimeDelta;
		prepare.reset = s.resetHistory;
		prepare.cameraNear = desc.cameraNear;
		prepare.cameraFar = desc.cameraFar;
		prepare.cameraFovAngleVertical = desc.cameraFovAngleVertical;
		prepare.viewSpaceToMetersFactor = 1;
		prepare.cameraUp[1] = prepare.cameraRight[0] = prepare.cameraForward[2] = 1;
		s.prepared = s.dispatch(&s.generation, &prepare.header) == FFX_API_RETURN_OK;
		if (!s.prepared)
		{
			s.Fail("FG prepare failed", true);
		}
	}
	r.InvalidateDrawState();
	return true;
#else
	(void)color;
	(void)colorState;
	(void)output;
	(void)outputState;
	(void)depth;
	return false;
#endif
}
void D3D12Ffx::PreparePresent(bool paused)
{
#ifdef BOTW_HAS_FFX
	auto &s = *impl;
	s.generating = false;
	if (s.callbackFailed.exchange(false))
		s.Fail("FG callback dispatch failed", true);
	if (!s.generation || !s.swap)
		return;
	ffxConfigureDescFrameGeneration config{};
	config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
	config.swapChain = s.wrapped.Get();
	const bool eligible = (paused || s.prepared) && s.fgPolicy.update(s.realMs, s.ModeHz(), paused, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count());
	config.frameGenerationEnabled = !s.failed && !s.fgFailed && s.prepared && eligible;
	config.frameGenerationCallback = Impl::Generate;
	config.frameGenerationCallbackUserContext = &s;
	config.frameID = s.frame;
	config.generationRect = {0, 0, static_cast<int32_t>(s.chain->GetWidth()), static_cast<int32_t>(s.chain->GetHeight())};
	if (s.configure(&s.generation, &config.header) != FFX_API_RETURN_OK)
		s.Fail("FG configuration failed", true);
	else
		s.generating = config.frameGenerationEnabled;
	if (paused)
	{
		ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{};
		wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
		if (s.dispatch(&s.swap, &wait.header) != FFX_API_RETURN_OK)
			s.Fail("FG pause present drain failed", true);
		s.previous = {};
	}
	s.prepared = false;
#else
	(void)paused;
#endif
}
void D3D12Ffx::EndFrame()
{
#ifdef BOTW_HAS_FFX
	if (impl->failed)
		Reset();
	else if (impl->fgFailed)
		impl->ResetGeneration();
#endif
}
bool D3D12Ffx::Generating() const
{
#ifdef BOTW_HAS_FFX
	return impl->generating;
#else
	return false;
#endif
}
#endif
