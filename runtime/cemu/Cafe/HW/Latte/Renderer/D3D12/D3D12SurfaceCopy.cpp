// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Ported from PrismFox67/Cemu-UWP; see runtime/cemu/VENDORED.md.

// Draw-based copies: presenting the TV/DRC image to the swap chain and depth <-> color surface copies.
// Both reuse the GLSL shaders written for the Vulkan backend.

// BotWRecomp: iOS also globs this directory; keep non-D3D12 builds platform-neutral.
#ifdef ENABLE_D3D12

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12InternalPipelines.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Fsr.h"
#include "../../../../../../src/fsr.h"
#include "../../../../../../src/fsr3_policy.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelineCache.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12SwapChain.h"
#include "Cafe/HW/Latte/Renderer/D3D12/RendererShaderD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureViewD3D12.h"
#include "Cafe/HW/Latte/Renderer/RendererOuputShader.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"

// Same GLSL as _vkGenSurfaceCopyShader_vs / _ps_depthToColor / _ps_colorToDepth in VulkanSurfaceCopy.cpp, except that
// the four vec2 corners are packed into two vec4: a vec2 array has a 8 byte stride in Vulkan push constants (std430)
// but HLSL constant buffer arrays always have a 16 byte stride. The memory layout is unchanged (8 floats, then ivec2)
static const char *s_copySurfaceVS =
	"#version 450\r\n"
	"layout(location = 0) out flat ivec2 passSrcTexelOffset;\r\n"
	"layout(push_constant) uniform pushConstants {\r\n"
	"vec4 vertexOffsets[2];\r\n"
	"ivec2 srcTexelOffset;\r\n"
	"}uf_pushConstants;\r\n"
	"\r\n"
	"void main(){\r\n"
	"vec2 tPOS;\r\n"
	"switch(gl_VertexIndex)"
	"{\r\n"
	"case 0: tPOS = uf_pushConstants.vertexOffsets[0].xy; break;\r\n"
	"case 1: tPOS = uf_pushConstants.vertexOffsets[0].zw; break;\r\n"
	"case 2: tPOS = uf_pushConstants.vertexOffsets[1].zw; break;\r\n"
	"case 3: tPOS = uf_pushConstants.vertexOffsets[0].xy; break;\r\n"
	"case 4: tPOS = uf_pushConstants.vertexOffsets[1].xy; break;\r\n"
	"case 5: tPOS = uf_pushConstants.vertexOffsets[1].zw; break;\r\n"
	"}"
	"passSrcTexelOffset = uf_pushConstants.srcTexelOffset;\r\n"
	"gl_Position = vec4(tPOS, 0, 1.0);\r\n"
	"}\r\n";

static const char *s_copySurfacePSColorToDepth =
	"#version 450\r\n"
	"layout(location = 0) in flat ivec2 passSrcTexelOffset;\r\n"
	"layout(binding = 0) uniform sampler2D textureSrc;\r\n"
	"in vec4 gl_FragCoord;\r\n"
	"\r\n"
	"void main(){\r\n"
	"gl_FragDepth = texelFetch(textureSrc, passSrcTexelOffset + ivec2(gl_FragCoord.xy), 0).r;\r\n"
	"}\r\n";

static const char *s_copySurfacePSDepthToColor =
	"#version 450\r\n"
	"layout(location = 0) in flat ivec2 passSrcTexelOffset;\r\n"
	"layout(binding = 0) uniform sampler2D textureSrc;\r\n"
	"layout(location = 0) out vec4 colorOut0;\r\n"
	"in vec4 gl_FragCoord;\r\n"
	"\r\n"
	"void main(){\r\n"
	"colorOut0.r = texelFetch(textureSrc, passSrcTexelOffset + ivec2(gl_FragCoord.xy), 0).r;\r\n"
	"}\r\n";

void D3D12Renderer::surfaceCopy_init()
{
	m_internal = std::make_unique<InternalPipelines>();
	m_internal->copyVS.reset(CreateInternalShader(RendererShader::ShaderType::kVertex, s_copySurfaceVS));
	m_internal->copyPSDepthToColor.reset(CreateInternalShader(RendererShader::ShaderType::kFragment, s_copySurfacePSDepthToColor));
	m_internal->copyPSColorToDepth.reset(CreateInternalShader(RendererShader::ShaderType::kFragment, s_copySurfacePSColorToDepth));
}

void D3D12Renderer::surfaceCopy_shutdown()
{
	m_internal.reset();
}

void D3D12Renderer::surfaceCopy_notifyViewRelease(LatteTextureViewD3D12 *view)
{
	// surface copies use temporary descriptors, nothing is cached per view
}

static D3D12_SAMPLER_DESC _MakeSampler(bool linear)
{
	D3D12_SAMPLER_DESC s{};
	s.Filter = linear ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
	s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	s.MaxAnisotropy = 1;
	s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	s.MaxLOD = D3D12_FLOAT32_MAX;
	return s;
}

void D3D12Renderer::DrawBackbufferQuad(LatteTextureView *texView, RendererOutputShader *shader, bool useLinearTexFilter,
									   sint32 imageX, sint32 imageY, sint32 imageWidth, sint32 imageHeight, bool padView, bool clearBackground)
{
	m_pipeline.CheckFront();
	auto *view = static_cast<LatteTextureViewD3D12 *>(texView);
	auto *vs = static_cast<RendererShaderD3D12 *>(shader->GetVertexShader());
	auto *ps = static_cast<RendererShaderD3D12 *>(shader->GetFragmentShader());
	for (auto *native : {vs, ps})
		if (!native->recordingId)
		{
			native->recordingId = RegisterResource(native);
			native->recordingOwner = this;
		}
	sint32 inputWidth, inputHeight;
	view->baseTexture->GetEffectiveSize(inputWidth, inputHeight, view->GetNativeDefinition().firstMip);
	const std::string_view overrideValue = fsr::upscaler_override();
	// Off unless the player (or BOTW_UPSCALER) picked FSR 1 or FSR 3.1.
	const auto requested = overrideValue == "off" ? fsr::Upscaler::Off : fsr::Upscaler::Spatial;
	OutputPacket packet{};
	packet.view = view->recordingId;
	packet.vertexShader = vs->recordingId;
	packet.pixelShader = ps->recordingId;
	packet.uniforms[0] = shader->FillUniformBlockBuffer(*texView, {imageWidth, imageHeight}, padView);
	packet.uniforms[1] = shader->FillUniformBlockBuffer(*texView, {inputWidth, inputHeight}, padView);
	packet.imageX = imageX;
	packet.imageY = imageY;
	packet.imageWidth = imageWidth;
	packet.imageHeight = imageHeight;
	packet.inputWidth = inputWidth;
	packet.inputHeight = inputHeight;
	packet.upscaler = static_cast<uint32>(requested);
#ifdef BOTW_FFX_STATIC
	const auto selection = fsr3::select(fsr3::console.load(), false, overrideValue, true, true);
	packet.nativeAA = fsr3::native_aa(selection.path, inputWidth, inputHeight, imageWidth, imageHeight);
#endif
	packet.linear = useLinearTexFilter;
	packet.padView = padView;
	packet.clearBackground = clearBackground;
	m_operations.Execute<OutputPacket, &D3D12Renderer::output_record>(m_pipeline, LattePipeline::Opcode::Present, packet, *this);
	ApplyFrontCompletions();
}

void D3D12Renderer::output_record(const OutputPacket &packet, const LattePipeline::Arena &)
{
	m_pipeline.CheckBack();
	const bool padView = packet.padView, clearBackground = packet.clearBackground, useLinearTexFilter = packet.linear;
	const auto imageX = packet.imageX, imageY = packet.imageY, imageWidth = packet.imageWidth, imageHeight = packet.imageHeight;
	const auto inputWidth = packet.inputWidth, inputHeight = packet.inputHeight;
	if (!AcquireBackbuffer(!padView))
		return;
	auto *swapChain = GetSwapChainObj(!padView);
	auto *view = m_resources.Get<LatteTextureViewD3D12>(packet.view);
	auto *vs = m_resources.Get<RendererShaderD3D12>(packet.vertexShader);
	auto *ps = m_resources.Get<RendererShaderD3D12>(packet.pixelShader);
	auto *pso = m_pipelineCache->GetInternalPipeline(vs, ps, D3D12SwapChain::kFormat, DXGI_FORMAT_UNKNOWN, false, false);
	if (!pso)
		return;
	const auto requested = static_cast<fsr::Upscaler>(packet.upscaler);
	bool fsr = !padView && !m_fsrFailed && fsr::should_upscale(requested, inputWidth, inputHeight, imageWidth, imageHeight);
	fsr = fsr || (!padView && !m_fsrFailed && packet.nativeAA);
	if (fsr && (!m_fsr || !m_fsr->Matches(inputWidth, inputHeight, imageWidth, imageHeight)))
	{
		// Keep the user's output filter for this frame; resize between presents.
		m_fsrInputWidth = inputWidth;
		m_fsrInputHeight = inputHeight;
		m_fsrOutputWidth = imageWidth;
		m_fsrOutputHeight = imageHeight;
		m_fsrResizePending = true;
		fsr = false;
	}
	if (fsr)
		m_fsr->BeginInput();
	// acquire the sampler table before recording any state (see GetSamplerTable)
	D3D12_SAMPLER_DESC sampler = _MakeSampler(useLinearTexFilter);
	const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable = GetSamplerTable(&sampler, 1);

	TransitionTexture(view->GetBaseTexture(), view->GetNativeDefinition().firstMip, view->GetNativeDefinition().numMip, view->GetNativeDefinition().firstSlice, view->GetNativeDefinition().numSlice, LatteTextureD3D12::kShaderReadState);
	FlushBarriers();

	if (!m_state.rootSignatureBound)
		BindRootSignatureAndHeaps();
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = fsr ? m_fsr->InputRTV() : swapChain->GetCurrentRTV();
	m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	if (clearBackground && !fsr)
	{
		const float clearColor[4] = {0, 0, 0, 0};
		m_cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
	}
	D3D12_VIEWPORT vp{fsr ? 0.f : (float)imageX, fsr ? 0.f : (float)imageY,
					  fsr ? (float)inputWidth : (float)imageWidth, fsr ? (float)inputHeight : (float)imageHeight, 0.0f, 1.0f};
	D3D12_RECT scissor{0, 0, fsr ? inputWidth : (LONG)swapChain->GetWidth(), fsr ? inputHeight : (LONG)swapChain->GetHeight()};
	m_cmdList->RSSetViewports(1, &vp);
	m_cmdList->RSSetScissorRects(1, &scissor);
	m_cmdList->SetPipelineState(pso);
	m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	const auto &outputUniforms = packet.uniforms[fsr ? 1 : 0];
	D3D12UploadAllocation ub = AllocateUpload(AlignUp<uint64>(sizeof(outputUniforms), D3D12Const::kCBVAlignment), D3D12Const::kCBVAlignment);
	memcpy(ub.cpuPtr, &outputUniforms, sizeof(outputUniforms));

	const D3D12BindingRemap &remap = ps->GetBindingRemap();
	D3D12StageTableDesc table;
	if (sint32 reg = remap.GetCBV(1); reg >= 0) // "parameters" block, binding 1 in the Vulkan variant
	{
		table.cbvAddress[reg] = ub.gpuAddress;
		table.cbvSize[reg] = (uint32)sizeof(outputUniforms);
	}
	if (sint32 reg = remap.GetSRV(0); reg >= 0)
		table.srv[reg] = view->GetSRVRGBA();
	BindStageRootCBVs(table, D3D12Const::Stage::Pixel);
	m_cmdList->SetGraphicsRootDescriptorTable(D3D12Const::RootParamStageTable(D3D12Const::Stage::Pixel), WriteStageTable(table, D3D12Const::Stage::Pixel));
	m_cmdList->SetGraphicsRootDescriptorTable(D3D12Const::RootParamStageSamplers(D3D12Const::Stage::Pixel), samplerTable);
	uint32 runtimeData[D3D12Const::kRuntimeDataDwords]{};
	m_cmdList->SetGraphicsRoot32BitConstants(D3D12Const::kRootParamRuntimeData, D3D12Const::kRuntimeDataDwords, runtimeData, 0);

	m_cmdList->DrawInstanced(6, 1, 0, 0);
	if (fsr)
		m_fsr->Draw(*swapChain, imageX, imageY, imageWidth, imageHeight, clearBackground);

	swapChain->SetDefinedContent();
	InvalidateDrawState();
	m_hasRecordedWork = true;
	for (auto id : {packet.view, packet.vertexShader, packet.pixelShader, view->GetBaseTexture()->recordingId})
		m_resources.Use(id, m_operations.ActiveHeader().sequence, GetRecordingSubmissionId());
}

void D3D12Renderer::surfaceCopy_copySurfaceWithFormatConversion(LatteTexture *sourceTexture, sint32 srcMip, sint32 srcSlice, LatteTexture *destinationTexture, sint32 dstMip, sint32 dstSlice, sint32 width, sint32 height)
{
	sint32 effectiveCopyWidth = width;
	sint32 effectiveCopyHeight = height;
	LatteTexture_scaleToEffectiveSize(sourceTexture, &effectiveCopyWidth, &effectiveCopyHeight, 0);

	auto *srcTex = static_cast<LatteTextureD3D12 *>(sourceTexture);
	auto *dstTex = static_cast<LatteTextureD3D12 *>(destinationTexture);
	if (!LatteTexture_doesEffectiveRescaleRatioMatch(srcTex, srcMip, dstTex, dstMip))
	{
		cemuLog_logDebug(LogType::Force, "surfaceCopy_copySurfaceWithFormatConversion(): Mismatching dimensions");
		return;
	}
	if (Latte::GetFormatBits(srcTex->GetNativeDefinition().format) != Latte::GetFormatBits(dstTex->GetNativeDefinition().format))
	{
		cemuLog_logDebug(LogType::Force, "surfaceCopy_copySurfaceWithFormatConversion(): Mismatching BPP");
		return;
	}
	cemu_assert_debug(srcTex != dstTex);
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::SurfaceCopy;
	packet.resources[0] = srcTex->recordingId;
	packet.resources[1] = dstTex->recordingId;
	packet.words[0] = srcMip;
	packet.words[1] = srcSlice;
	packet.words[2] = dstMip;
	packet.words[3] = dstSlice;
	packet.words[4] = effectiveCopyWidth;
	packet.words[5] = effectiveCopyHeight;

	const auto reply = ExecuteBackend(LattePipeline::Opcode::Copy, packet);
	if (reply.words[0])
		LatteTexture_TrackTextureGPUWrite(dstTex, dstSlice, dstMip, LatteTexture_getNextUpdateEventCounter());
}

void D3D12Renderer::surfaceCopy_record(const BackendPacket &packet)
{
	m_pipeline.CheckBack();
	auto *srcTex = m_resources.Get<LatteTextureD3D12>(packet.resources[0]);
	auto *dstTex = m_resources.Get<LatteTextureD3D12>(packet.resources[1]);
	const sint32 srcMip = packet.words[0], srcSlice = packet.words[1], dstMip = packet.words[2], dstSlice = packet.words[3];
	const sint32 effectiveCopyWidth = packet.words[4], effectiveCopyHeight = packet.words[5];
	RendererShaderD3D12 *ps = dstTex->GetNativeDefinition().isDepth ? m_internal->copyPSColorToDepth.get() : m_internal->copyPSDepthToColor.get();
	const DXGI_FORMAT rtvFormat = dstTex->GetNativeDefinition().isDepth ? DXGI_FORMAT_UNKNOWN : dstTex->GetFormatInfo().rtvFormat;
	const DXGI_FORMAT dsvFormat = dstTex->GetNativeDefinition().isDepth ? dstTex->GetFormatInfo().dsvFormat : DXGI_FORMAT_UNKNOWN;
	if (!dstTex->GetNativeDefinition().isDepth && rtvFormat == DXGI_FORMAT_UNKNOWN)
	{
		cemuLog_logDebugOnce(LogType::Force, "D3D12: Surface copy into non-renderable format {:04x}", (uint32)dstTex->GetNativeDefinition().format);
		return;
	}
	ID3D12PipelineState *pso = m_pipelineCache->GetInternalPipeline(m_internal->copyVS.get(), ps, rtvFormat, dsvFormat, dstTex->GetNativeDefinition().isDepth, false);
	if (!pso || !srcTex->GetResource() || !dstTex->GetResource())
		return;
	// acquire the sampler table before recording any state (see GetSamplerTable)
	D3D12_SAMPLER_DESC sampler = _MakeSampler(false);
	const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable = GetSamplerTable(&sampler, 1);

	// source SRV of a single mip/slice. Depth textures are read through their depth SRV format
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = srcTex->GetFormatInfo().srvFormat;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Texture2DArray.MostDetailedMip = srcMip;
	srvDesc.Texture2DArray.MipLevels = 1;
	srvDesc.Texture2DArray.FirstArraySlice = srcSlice;
	srvDesc.Texture2DArray.ArraySize = 1;
	if (srcSlice == 0)
	{
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = srcMip;
		srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Texture2D.PlaneSlice = 0;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE srv = m_stagingViewHeap->Allocate();
	m_device->CreateShaderResourceView(srcTex->GetResource(), &srvDesc, srv);

	TransitionTexture(srcTex, srcMip, 1, srcSlice, 1, LatteTextureD3D12::kShaderReadState);
	TransitionTexture(dstTex, dstMip, 1, dstSlice, 1, dstTex->GetNativeDefinition().isDepth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
	FlushBarriers();

	D3D12_CPU_DESCRIPTOR_HANDLE target{};
	if (dstTex->GetNativeDefinition().isDepth)
	{
		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		dsvDesc.Format = dsvFormat;
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		dsvDesc.Texture2DArray.MipSlice = dstMip;
		dsvDesc.Texture2DArray.FirstArraySlice = dstSlice;
		dsvDesc.Texture2DArray.ArraySize = 1;
		target = m_stagingDSVHeap->Allocate();
		m_device->CreateDepthStencilView(dstTex->GetResource(), &dsvDesc, target);
		m_cmdList->OMSetRenderTargets(0, nullptr, FALSE, &target);
	}
	else
	{
		D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
		rtvDesc.Format = rtvFormat;
		rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
		rtvDesc.Texture2DArray.MipSlice = dstMip;
		rtvDesc.Texture2DArray.FirstArraySlice = dstSlice;
		rtvDesc.Texture2DArray.ArraySize = 1;
		target = m_stagingRTVHeap->Allocate();
		m_device->CreateRenderTargetView(dstTex->GetResource(), &rtvDesc, target);
		m_cmdList->OMSetRenderTargets(1, &target, FALSE, nullptr);
	}

	if (!m_state.rootSignatureBound)
		BindRootSignatureAndHeaps();
	D3D12_VIEWPORT vp{0.0f, 0.0f, (float)effectiveCopyWidth, (float)effectiveCopyHeight, 0.0f, 1.0f};
	D3D12_RECT scissor{0, 0, effectiveCopyWidth, effectiveCopyHeight};
	m_cmdList->RSSetViewports(1, &vp);
	m_cmdList->RSSetScissorRects(1, &scissor);
	m_cmdList->SetPipelineState(pso);
	m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	struct
	{
		float vertexOffsets[4 * 2];
		sint32 srcTexelOffset[2];
	} pushConstants = {{-1.0f, 1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f}, {0, 0}};
	SetPushConstants(&pushConstants, sizeof(pushConstants));

	const D3D12BindingRemap &remap = ps->GetBindingRemap();
	D3D12StageTableDesc table;
	if (sint32 reg = remap.GetSRV(0); reg >= 0)
		table.srv[reg] = srv;
	BindStageRootCBVs(table, D3D12Const::Stage::Pixel);
	m_cmdList->SetGraphicsRootDescriptorTable(D3D12Const::RootParamStageTable(D3D12Const::Stage::Pixel), WriteStageTable(table, D3D12Const::Stage::Pixel));
	m_cmdList->SetGraphicsRootDescriptorTable(D3D12Const::RootParamStageSamplers(D3D12Const::Stage::Pixel), samplerTable);
	uint32 runtimeData[D3D12Const::kRuntimeDataDwords]{};
	m_cmdList->SetGraphicsRoot32BitConstants(D3D12Const::kRootParamRuntimeData, D3D12Const::kRuntimeDataDwords, runtimeData, 0);

	m_cmdList->DrawInstanced(6, 1, 0, 0);

	// descriptors were consumed when recording (RTV/DSV) or copied into the shader visible heap (SRV)
	m_stagingViewHeap->Free(srv);
	if (dstTex->GetNativeDefinition().isDepth)
		m_stagingDSVHeap->Free(target);
	else
		m_stagingRTVHeap->Free(target);

	InvalidateDrawState();
	m_hasRecordedWork = true;
	m_operations.Result().words[0] = true;
}

#endif // ENABLE_D3D12
