// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Ported from PrismFox67/Cemu-UWP; see runtime/cemu/VENDORED.md.

// Draw path of the D3D12 renderer: uniform data, descriptor tables, samplers, pipeline binding and draw submission.
// Structure and register interpretation follow VulkanRendererCore.cpp.

// BotWRecomp: iOS also globs this directory; keep non-D3D12 builds platform-neutral.
#ifdef ENABLE_D3D12

#include "Cafe/HW/Latte/Core/LatteProfiler.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ShaderCompiler.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelineCache.h"
#include "Cafe/HW/Latte/Renderer/D3D12/RendererShaderD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureViewD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/CachedFBOD3D12.h"
#include "Cafe/HW/Latte/Renderer/RendererCore.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/OS/libs/gx2/GX2.h"

#include <bit>

#ifdef CEMU_D3D12_SPIRV_TO_DXIL
extern "C"
{
#include "spirv_to_dxil.h"
}
#endif

extern bool hasValidFramebufferAttached;

#ifdef CEMU_D3D12_SPIRV_TO_DXIL
static_assert(sizeof(dxil_spirv_vertex_runtime_data) <= D3D12Const::kRuntimeDataDwords * 4, "runtime data root constants too small");
#endif

/* --- sequence handling --- */

void D3D12Renderer::draw_beginSequence()
{
	LatteProfiler::DetailScope detailScope(LatteProfiler::Detail::Sequence);
	ServicePendingPresent(false); // frame interpolation without vsync: the previous real frame may be due
	LatteProfiler::Scope decodeScope(LatteProfiler::Slot::DrawStateDecode);
	// BotWRecomp: Cemu frontend cache trimming is not present in this runtime.
	m_state.skipDrawSequence = false;
	const bool streamoutEnable = LatteGPUState.contextRegister[mmVGT_STRMOUT_EN] != 0;
	m_state.streamoutEnabled = streamoutEnable;
	if (!streamoutEnable)
		LatteStreamout_Disable();

	{
		LatteProfiler::DetailScope detailScope(LatteProfiler::Detail::ShaderResolve);
		LatteSHRC_UpdateActiveShaders();
	}
	if (LatteGPUState.activeShaderHasError)
	{
		cemuLog_logDebugOnce(LogType::Force, "Skipping drawcalls due to shader error");
		m_state.skipDrawSequence = true;
		return;
	}

	LatteGPUState.requiresTextureBarrier = false;
	while (true)
	{
		LatteGPUState.repeatTextureInitialization = false;
		if (!LatteMRT::UpdateCurrentFBO())
		{
			m_state.skipDrawSequence = true;
			return;
		}
		if (!hasValidFramebufferAttached && !streamoutEnable)
		{
			m_state.skipDrawSequence = true;
			return;
		}
		LatteTexture_updateTextures();
		if (!LatteGPUState.repeatTextureInitialization)
			break;
	}
	LatteMRT::ApplyCurrentState();
	LatteRenderTarget_updateViewport();
	LatteRenderTarget_updateScissorBox();

	bool rasterizerEnable = LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() == false;
	if (!LatteGPUState.contextNew.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA())
		rasterizerEnable = true;
	if (!rasterizerEnable && !streamoutEnable)
		m_state.skipDrawSequence = true;
}

void D3D12Renderer::draw_endSequence()
{
	LatteDecompilerShader *pixelShader = LatteSHRC_GetActivePixelShader();
	if (pixelShader)
		LatteRenderTarget_trackUpdates();
	const bool hasReadback = LatteTextureReadback_Update();
	if (m_drawsInCommandList >= kDrawsPerSubmission || hasReadback || m_uploadRing->GetRecordingOverflowBytes() > 0)
		SubmitCommandList(false);
}

void D3D12Renderer::draw_handleSpecialState5()
{
	LatteMRT::UpdateCurrentFBO();
	LatteRenderTarget_updateViewport();
	LatteTextureView *colorBuffer = LatteMRT::GetColorAttachment(0);
	LatteTextureView *depthBuffer = LatteMRT::GetDepthAttachment();
	if (!colorBuffer || !depthBuffer)
		return;
	sint32 vpWidth, vpHeight;
	LatteMRT::GetVirtualViewportDimensions(vpWidth, vpHeight);
	surfaceCopy_copySurfaceWithFormatConversion(
		depthBuffer->baseTexture, depthBuffer->firstMip, depthBuffer->firstSlice,
		colorBuffer->baseTexture, colorBuffer->firstMip, colorBuffer->firstSlice,
		vpWidth, vpHeight);
}

/* --- uniforms --- */

bool D3D12Renderer::draw_prepareUniformVars(LatteDecompilerShader *shader, D3D12_GPU_VIRTUAL_ADDRESS &addressOut, uint32 &sizeOut)
{
	LatteProfiler::Scope profileScope(LatteProfiler::Slot::Uniforms);
	addressOut = 0;
	sizeOut = 0;
	if (shader->resourceMapping.uniformVarsBufferBindingPoint < 0)
		return false;
	const uint32 rangeSize = shader->uniform.uniformRangeSize;
	if (m_uniformScratch.size() < AlignUp<uint32>(rangeSize, 16))
		m_uniformScratch.resize(AlignUp<uint32>(rangeSize, 16) + 256);
	float *uniformBuf = (float *)m_uniformScratch.data();
	auto GET_UNIFORM_DATA_PTR = [uniformBuf](size_t index) { return uniformBuf + (index / 4); };

	for (auto &entry : shader->uniform.list_ufTexRescale)
	{
		float *xyScale = LatteTexture_getEffectiveTextureScale(shader->shaderType, entry.texUnit);
		memcpy(entry.currentValue, xyScale, sizeof(float) * 2);
		memcpy(GET_UNIFORM_DATA_PTR(entry.uniformLocation), xyScale, sizeof(float) * 2);
	}
	if (shader->uniform.loc_alphaTestRef >= 0)
		*GET_UNIFORM_DATA_PTR(shader->uniform.loc_alphaTestRef) = LatteGPUState.contextNew.SX_ALPHA_REF.get_ALPHA_TEST_REF();
	if (shader->uniform.loc_pointSize >= 0)
	{
		const auto &pointSizeReg = LatteGPUState.contextNew.PA_SU_POINT_SIZE;
		float pointWidth = (float)pointSizeReg.get_WIDTH() / 8.0f;
		if (pointWidth == 0.0f)
			pointWidth = 1.0f / 8.0f;
		*GET_UNIFORM_DATA_PTR(shader->uniform.loc_pointSize) = pointWidth;
	}
	if (shader->uniform.loc_remapped >= 0)
		LatteBufferCache_LoadRemappedUniforms(shader, GET_UNIFORM_DATA_PTR(shader->uniform.loc_remapped), true, (1 << LATTE_NUM_MAX_UNIFORM_BUFFERS) - 1);
	if (shader->uniform.loc_uniformRegister >= 0)
	{
		sint32 shaderAluConst = 0;
		if (shader->shaderType == LatteConst::ShaderType::Vertex)
			shaderAluConst = 0x400;
		uint32 *uniformRegData = (uint32 *)(LatteGPUState.contextRegister + mmSQ_ALU_CONSTANT0_0 + shaderAluConst);
		memcpy(GET_UNIFORM_DATA_PTR(shader->uniform.loc_uniformRegister), uniformRegData, shader->uniform.count_uniformRegister * 16);
	}
	if (shader->uniform.loc_windowSpaceToClipSpaceTransform >= 0)
	{
		sint32 viewportWidth, viewportHeight;
		LatteRenderTarget_GetCurrentVirtualViewportSize(&viewportWidth, &viewportHeight);
		float *v = GET_UNIFORM_DATA_PTR(shader->uniform.loc_windowSpaceToClipSpaceTransform);
		v[0] = 2.0f / (float)viewportWidth;
		v[1] = 2.0f / (float)viewportHeight;
	}
	if (shader->uniform.loc_fragCoordScale >= 0)
		LatteMRT::GetCurrentFragCoordScale(GET_UNIFORM_DATA_PTR(shader->uniform.loc_fragCoordScale));
	if (shader->uniform.loc_verticesPerInstance >= 0)
	{
		*(int *)GET_UNIFORM_DATA_PTR(shader->uniform.loc_verticesPerInstance) = (int)m_state.verticesPerInstance;
		for (sint32 b = 0; b < LATTE_NUM_STREAMOUT_BUFFER; b++)
		{
			if (shader->uniform.loc_streamoutBufferBase[b] >= 0)
				*(uint32 *)GET_UNIFORM_DATA_PTR(shader->uniform.loc_streamoutBufferBase[b]) = m_state.streamoutBuffer[b].ringBufferOffset;
		}
	}
	auto &cached = m_uniformCache[(uint32)D3D12StageFromLatte(shader->shaderType)];
	if (cached.submission == m_lastSubmittedFenceValue && cached.data.size() == rangeSize &&
		memcmp(cached.data.data(), uniformBuf, rangeSize) == 0)
	{
		addressOut = cached.address;
		sizeOut = rangeSize;
		return true;
	}
	D3D12UploadAllocation upload = AllocateUpload(AlignUp<uint32>(std::max<uint32>(rangeSize, 16), D3D12Const::kCBVAlignment), D3D12Const::kCBVAlignment);
	cached.data.assign((const uint8 *)uniformBuf, (const uint8 *)uniformBuf + rangeSize);
	cached.address = upload.gpuAddress;
	cached.submission = m_lastSubmittedFenceValue;
	memcpy(upload.cpuPtr, uniformBuf, rangeSize);
	addressOut = upload.gpuAddress;
	sizeOut = rangeSize;
	return true;
}

/* --- samplers --- */

void D3D12Renderer::draw_buildSamplerDesc(LatteDecompilerShader *shader, uint32 relativeTextureUnit, LatteTextureViewD3D12 *view, D3D12_SAMPLER_DESC &desc)
{
	const uint32 stageSamplerIndex = shader->textureUnitSamplerAssignment[relativeTextureUnit];
	if (stageSamplerIndex == LATTE_DECOMPILER_SAMPLER_NONE)
	{
		desc = {};
		desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		desc.AddressU = desc.AddressV = desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		desc.MaxAnisotropy = 1;
		desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		desc.MinLOD = 0.0f;
		desc.MaxLOD = 0.0f; // matches the zero initialized VkSamplerCreateInfo the Vulkan backend uses when no sampler is assigned

		return;
	}
	LatteTextureD3D12 *baseTexture = view ? view->GetBaseTexture() : nullptr;
	const uint32 samplerIndex = stageSamplerIndex + LatteDecompiler_getTextureSamplerBaseIndex(shader->shaderType);
	const _LatteRegisterSetSampler *samplerWords = LatteGPUState.contextNew.SQ_TEX_SAMPLER + samplerIndex;

	const _LatteRegisterSetSamplerBorderColor *borderColorReg;
	if (shader->shaderType == LatteConst::ShaderType::Vertex)
		borderColorReg = LatteGPUState.contextNew.TD_VS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
	else if (shader->shaderType == LatteConst::ShaderType::Pixel)
		borderColorReg = LatteGPUState.contextNew.TD_PS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
	else
		borderColorReg = LatteGPUState.contextNew.TD_GS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
	// Three sampler words, four border channels, depth compare and five texture override words.
	std::array<uint32, 13> key{};
	memcpy(key.data(), samplerWords, sizeof(*samplerWords));
	memcpy(key.data() + 3, borderColorReg, sizeof(*borderColorReg));
	key[7] = shader->textureUsesDepthCompare[relativeTextureUnit];
	if (baseTexture)
	{
		const auto &overrides = baseTexture->overwriteInfo;
		key[8] = overrides.hasRelativeLodBias;
		key[9] = overrides.relativeLodBias;
		key[10] = overrides.hasLodBias;
		key[11] = overrides.lodBias;
		key[12] = (uint32)overrides.anisotropicLevel;
	}
	else
		key[12] = ~0u;
	auto &cached = m_samplerDescCache[(uint32)D3D12StageFromLatte(shader->shaderType)][relativeTextureUnit];
	if (cached.valid && cached.key == key)
	{
		desc = cached.desc;
		return;
	}

	desc = {};
	desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	desc.AddressU = desc.AddressV = desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	desc.MaxAnisotropy = 1;
	desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	desc.MinLOD = 0.0f;
	desc.MaxLOD = 0.0f; // matches the zero initialized VkSamplerCreateInfo the Vulkan backend uses when no sampler is assigned

	// LOD
	const uint32 iMinLOD = samplerWords->WORD1.get_MIN_LOD();
	const uint32 iMaxLOD = samplerWords->WORD1.get_MAX_LOD();
	sint32 iLodBias = samplerWords->WORD1.get_LOD_BIAS();
	if (baseTexture)
	{
		if (baseTexture->overwriteInfo.hasRelativeLodBias)
			iLodBias += baseTexture->overwriteInfo.relativeLodBias;
		if (baseTexture->overwriteInfo.hasLodBias)
			iLodBias = baseTexture->overwriteInfo.lodBias;
	}
	using ZF = Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER;
	using XYF = Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER;
	const auto filterMip = samplerWords->WORD0.get_MIP_FILTER();
	D3D12_FILTER_TYPE mipFilter = D3D12_FILTER_TYPE_LINEAR;
	if (filterMip == ZF::NONE)
	{
		mipFilter = D3D12_FILTER_TYPE_POINT;
		desc.MinLOD = 0.0f;
		desc.MaxLOD = 0.25f;
	}
	else
	{
		mipFilter = (filterMip == ZF::POINT) ? D3D12_FILTER_TYPE_POINT : D3D12_FILTER_TYPE_LINEAR;
		desc.MinLOD = (float)iMinLOD / 64.0f;
		desc.MaxLOD = (float)iMaxLOD / 64.0f;
	}
	const auto filterMin = samplerWords->WORD0.get_XY_MIN_FILTER();
	const auto filterMag = samplerWords->WORD0.get_XY_MAG_FILTER();
	const D3D12_FILTER_TYPE minFilter = (filterMin == XYF::POINT || filterMin == XYF::ANISO_POINT) ? D3D12_FILTER_TYPE_POINT : D3D12_FILTER_TYPE_LINEAR;
	const D3D12_FILTER_TYPE magFilter = (filterMag == XYF::POINT || filterMag == XYF::ANISO_POINT) ? D3D12_FILTER_TYPE_POINT : D3D12_FILTER_TYPE_LINEAR;

	desc.AddressU = D3D12Format::GetAddressMode(samplerWords->WORD0.get_CLAMP_X());
	desc.AddressV = D3D12Format::GetAddressMode(samplerWords->WORD0.get_CLAMP_Y());
	desc.AddressW = D3D12Format::GetAddressMode(samplerWords->WORD0.get_CLAMP_Z());

	sint32 maxAniso = samplerWords->WORD0.get_MAX_ANISO_RATIO();
	if (baseTexture && baseTexture->overwriteInfo.anisotropicLevel >= 0)
		maxAniso = baseTexture->overwriteInfo.anisotropicLevel;
	desc.MipLODBias = std::clamp((float)iLodBias / 64.0f, -16.0f, 15.99f);

	const bool depthCompare = shader->textureUsesDepthCompare[relativeTextureUnit];
	const D3D12_FILTER_REDUCTION_TYPE reduction = depthCompare ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON : D3D12_FILTER_REDUCTION_TYPE_STANDARD;
	if (depthCompare)
		desc.ComparisonFunc = D3D12Format::GetCompareFunc((uint32)samplerWords->WORD0.get_DEPTH_COMPARE_FUNCTION());

	if (maxAniso > 0 && minFilter == D3D12_FILTER_TYPE_LINEAR)
	{
		desc.Filter = D3D12_ENCODE_ANISOTROPIC_FILTER(reduction);
		desc.MaxAnisotropy = std::min<UINT>(1u << maxAniso, 16);
	}
	else
	{
		desc.Filter = D3D12_ENCODE_BASIC_FILTER(minFilter, magFilter, mipFilter, reduction);
		desc.MaxAnisotropy = 1;
	}

	// border color. D3D12 supports arbitrary border colors natively
	const auto borderType = samplerWords->WORD0.get_BORDER_COLOR_TYPE();
	using BT = Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE;
	if (borderType == BT::TRANSPARENT_BLACK)
	{
		desc.BorderColor[0] = desc.BorderColor[1] = desc.BorderColor[2] = desc.BorderColor[3] = 0.0f;
	}
	else if (borderType == BT::OPAQUE_BLACK)
	{
		desc.BorderColor[0] = desc.BorderColor[1] = desc.BorderColor[2] = 0.0f;
		desc.BorderColor[3] = 1.0f;
	}
	else if (borderType == BT::OPAQUE_WHITE)
	{
		desc.BorderColor[0] = desc.BorderColor[1] = desc.BorderColor[2] = desc.BorderColor[3] = 1.0f;
	}
	else
	{
		desc.BorderColor[0] = borderColorReg->red.get_channelValue();
		desc.BorderColor[1] = borderColorReg->green.get_channelValue();
		desc.BorderColor[2] = borderColorReg->blue.get_channelValue();
		desc.BorderColor[3] = borderColorReg->alpha.get_channelValue();
	}
	cached.key = key;
	cached.desc = desc;
	cached.valid = true;
}

/* --- descriptor tables --- */

void D3D12Renderer::BindStageRootCBVs(const D3D12StageTableDesc &bindings, D3D12Const::Stage stage)
{
	for (uint32 i = 0; i < D3D12Const::RootCBVCount(stage); ++i)
	{
		// Always replace unused registers with a permanent 64-KiB zero buffer. Keeping
		// an old upload address could expose retired memory when the next shader changes.
		const auto address = bindings.cbvAddress[i] ? bindings.cbvAddress[i] : m_zeroConstantBuffer->GetGPUVirtualAddress();
		cemu_assert_debug(address % D3D12Const::kCBVAlignment == 0);
		auto &current = m_state.currentRootCBVs[(uint32)stage][i];
		if (current == address)
			LatteProfiler::Add(LatteProfiler::Counter::RootCBVSkips);
		else
		{
			m_cmdList->SetGraphicsRootConstantBufferView(D3D12Const::RootParamStageRootCBV(stage, i), address);
			current = address;
			LatteProfiler::Add(LatteProfiler::Counter::RootCBVSets);
		}
	}
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::WriteStageTable(const D3D12StageTableDesc &tableDesc, D3D12Const::Stage stage)
{
	LatteProfiler::Scope profileScope(LatteProfiler::Slot::Descriptors);
	using namespace D3D12Const;
	const auto key = tableDesc.TableKey(stage);
	auto &last = m_lastViewTable[(uint32)stage];
	auto &cache = m_viewTableCache[(uint32)stage];
	const auto generation = m_gpuViewHeap->GetGeneration();
	const auto sourceGeneration = m_stagingViewHeap->GetGeneration();
	if (const auto *table = last.Find(key, generation, sourceGeneration))
	{
		LatteProfiler::Add(LatteProfiler::Counter::DescriptorHits);
		LatteProfiler::Add(LatteProfiler::Counter::DescriptorFastHits);
		return *table;
	}
	if (const auto *table = cache.Find(key, generation, sourceGeneration))
	{
		LatteProfiler::Add(LatteProfiler::Counter::DescriptorHits);
		last.Store(key, *table, generation, sourceGeneration);
		return *table;
	}
	LatteProfiler::Add(LatteProfiler::Counter::DescriptorMisses);

	const UINT tableSize = StageTableSize(stage);
	auto table = m_gpuViewHeap->AllocateTable(tableSize);
	// Copy stable CBVs, SRVs and null slots in one call; only changed CBVs need to be created.
	D3D12_CPU_DESCRIPTOR_HANDLE sources[kStageTableSize];
	for (uint32 i = RootCBVCount(stage); i < kMaxCBVsPerStage; ++i)
	{
		auto &cbv = m_cbvCache[(uint32)stage][i];
		if (!tableDesc.cbvAddress[i])
		{
			sources[kStageTableOffsetCBV + i - RootCBVCount(stage)] = m_nullCBV;
			continue;
		}
		if (!cbv.descriptor.ptr)
			cbv.descriptor = m_stagingViewHeap->Allocate();
		if (cbv.address != tableDesc.cbvAddress[i] || cbv.size != tableDesc.cbvSize[i])
		{
			WriteCBV(cbv.descriptor, tableDesc.cbvAddress[i], tableDesc.cbvSize[i]);
			cbv.address = tableDesc.cbvAddress[i];
			cbv.size = tableDesc.cbvSize[i];
		}
		sources[kStageTableOffsetCBV + i - RootCBVCount(stage)] = cbv.descriptor;
	}
	for (uint32 i = 0; i < kMaxSRVsPerStage; ++i)
		sources[StageTableOffsetSRV(stage) + i] = tableDesc.srv[i].ptr ? tableDesc.srv[i] : m_nullSRV2D;
	auto &uav = m_stageUAVCache[(uint32)stage];
	sources[StageTableOffsetUAV(stage)] = m_nullUAV;
	if (tableDesc.uavResource)
	{
		if (!uav.descriptor.ptr)
			uav.descriptor = m_stagingViewHeap->Allocate();
		if (uav.resource != tableDesc.uavResource || uav.size != tableDesc.uavSize)
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
			desc.Format = DXGI_FORMAT_R32_TYPELESS;
			desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
			desc.Buffer.NumElements = tableDesc.uavSize / 4;
			desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
			m_device->CreateUnorderedAccessView(tableDesc.uavResource, nullptr, &desc, uav.descriptor);
			uav.resource = tableDesc.uavResource;
			uav.size = tableDesc.uavSize;
		}
		sources[StageTableOffsetUAV(stage)] = uav.descriptor;
	}
	m_device->CopyDescriptors(1, &table.cpu, &tableSize, tableSize, sources, nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	// AllocateTable can advance the ring generation: store only the post-allocation epoch.
	const auto newGeneration = m_gpuViewHeap->GetGeneration();
	const auto newSourceGeneration = m_stagingViewHeap->GetGeneration();
	last.Store(key, table.gpu, newGeneration, newSourceGeneration);
	cache.Store(key, table.gpu, newGeneration, newSourceGeneration);
	return table.gpu;
}

static LatteTextureViewD3D12 *_GetBoundTextureForShader(LatteTextureViewD3D12 **boundTextures, LatteDecompilerShader *shader, uint32 relativeTextureUnit, uint32 &texUnitRegIndexOut)
{
	uint32 hostTextureUnit = relativeTextureUnit;
	uint32 texUnitRegIndex = relativeTextureUnit * 7;
	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		hostTextureUnit += LATTE_CEMU_VS_TEX_UNIT_BASE;
		texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS;
		break;
	case LatteConst::ShaderType::Pixel:
		hostTextureUnit += LATTE_CEMU_PS_TEX_UNIT_BASE;
		texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
		break;
	case LatteConst::ShaderType::Geometry:
		hostTextureUnit += LATTE_CEMU_GS_TEX_UNIT_BASE;
		texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS;
		break;
	default:
		break;
	}
	texUnitRegIndexOut = texUnitRegIndex;
	return boundTextures[hostTextureUnit];
}

void D3D12Renderer::draw_transitionInputTextures(LatteDecompilerShader *shader)
{
	const sint32 textureCount = shader->resourceMapping.getTextureCount();
	for (sint32 i = 0; i < textureCount; i++)
	{
		const uint32 relativeTextureUnit = shader->resourceMapping.getRelativeTextureUnitFromRelativeBindingPoint(i);
		uint32 regIndex;
		LatteTextureViewD3D12 *view = _GetBoundTextureForShader(m_state.boundTexture, shader, relativeTextureUnit, regIndex);
		if (!view)
			continue;
		if (view->ReuseShaderReadTransition())
		{
			LatteProfiler::Add(LatteProfiler::Counter::TransitionHits);
			continue;
		}
		TransitionTexture(view->GetBaseTexture(), view->firstMip, view->numMip, view->firstSlice, view->numSlice, LatteTextureD3D12::kShaderReadState);
		view->RememberShaderReadTransition();
	}
}

void D3D12Renderer::draw_buildStageBindings(LatteDecompilerShader *shader, RendererShaderD3D12 *hostShader, PreparedStageBindings &out)
{
	LatteProfiler::Scope profileScope(LatteProfiler::Slot::Descriptors);
	using namespace D3D12Const;
	const Stage stage = D3D12StageFromLatte(shader->shaderType);
	const D3D12BindingRemap &remap = hostShader->GetBindingRemap();
	auto &mapping = shader->resourceMapping; // accessors are not const
	out.stage = stage;
	out.tableDesc = D3D12StageTableDesc();
	D3D12StageTableDesc &tableDesc = out.tableDesc;

	// Persistent uniform buffers; transient uniform variables are prepared after sampler acquisition.
	const auto &stageBindings = m_state.stage[(uint32)stage];
	for (uint32 i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++)
	{
		if (mapping.uniformBuffersBindingPoint[i] < 0)
			continue;
		const sint32 reg = remap.GetCBV(mapping.uniformBuffersBindingPoint[i]);
		if (reg < 0 || !stageBindings.uniformBufferAddress[i])
			continue;
		tableDesc.cbvAddress[reg] = stageBindings.uniformBufferAddress[i];
		tableDesc.cbvSize[reg] = std::min<uint32>(stageBindings.uniformBufferSize[i], D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16);
	}

	{
		LatteProfiler::Scope textureScope(LatteProfiler::Slot::TextureBinding);
		// textures + samplers
		D3D12_SAMPLER_DESC *samplers = out.samplers;
		out.samplerCount = std::max<uint32>(remap.srvCount, 1);
		cemu_assert_debug(out.samplerCount <= kMaxSamplersPerStage);
		static const auto defaultSamplers = [] {
			std::array<D3D12_SAMPLER_DESC, kMaxSamplersPerStage> values{};
			for (auto &sampler : values)
			{
				sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
				sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				sampler.MaxAnisotropy = 1;
				sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
			}
			return values;
		}();
		memcpy(samplers, defaultSamplers.data(), out.samplerCount * sizeof(*samplers));
		out.samplerTable = {};
		const sint32 textureBase = mapping.getTextureBaseBindingPoint();
		const sint32 textureCount = mapping.getTextureCount();
		for (sint32 i = 0; i < textureCount && textureBase >= 0; i++)
		{
			const sint32 reg = remap.GetSRV(textureBase + i);
			if (reg < 0)
				continue;
			const uint32 relativeTextureUnit = mapping.getRelativeTextureUnitFromRelativeBindingPoint(i);
			const Latte::E_DIM shaderDim = shader->textureUnitDim[relativeTextureUnit];
			uint32 texUnitRegIndex;
			LatteTextureViewD3D12 *view = _GetBoundTextureForShader(m_state.boundTexture, shader, relativeTextureUnit, texUnitRegIndex);
			bool dimMismatch = false;
			if (view)
			{
				if (shaderDim == Latte::E_DIM::DIM_1D && view->dim != Latte::E_DIM::DIM_1D)
					dimMismatch = true;
				else if (shaderDim == Latte::E_DIM::DIM_2D && view->dim != Latte::E_DIM::DIM_2D && view->dim != Latte::E_DIM::DIM_2D_MSAA)
					dimMismatch = true;
			}
			if (!view || dimMismatch)
			{
				tableDesc.srv[reg] = GetNullSRV(shaderDim);
			}
			else
			{
				const uint32 word4 = LatteGPUState.contextRegister[texUnitRegIndex + 4];
				tableDesc.srv[reg] = view->GetSRV(word4);
			}
			draw_buildSamplerDesc(shader, relativeTextureUnit, view, samplers[reg]);
		}
	}

	// transform feedback via UAV
	if (mapping.tfStorageBindingPoint >= 0 && remap.GetUAV(mapping.tfStorageBindingPoint) == 0)
	{
		tableDesc.uavResource = m_xfbRingBuffer.Get();
		tableDesc.uavSize = m_xfbRingBufferSize;
	}
}

void D3D12Renderer::draw_acquireSamplerTables(PreparedStageBindings *stages, uint32 count)
{
	LatteProfiler::Scope profileScope(LatteProfiler::Slot::Descriptors);
	// all tables of a draw have to come from the same generation of the sampler heap
	for (int attempt = 0; attempt < 2; attempt++)
	{
		bool ok = true;
		for (uint32 i = 0; i < count && ok; i++)
		{
			auto &stage = stages[i];
			auto &cached = m_stageSamplerCache[(uint32)stage.stage];
			const size_t bytes = stage.samplerCount * sizeof(D3D12_SAMPLER_DESC);
			if (cached.generation == m_gpuSamplerHeap->GetGeneration() && cached.count == stage.samplerCount &&
				memcmp(cached.samplers, stage.samplers, bytes) == 0)
			{
				stage.samplerTable = cached.table;
				continue;
			}
			ok = TryGetSamplerTable(stage.samplers, stage.samplerCount, stage.samplerTable);
			if (ok)
			{
				memcpy(cached.samplers, stage.samplers, bytes);
				cached.count = stage.samplerCount;
				cached.table = stage.samplerTable;
				cached.generation = m_gpuSamplerHeap->GetGeneration();
			}
		}
		if (ok)
			return;
		cemuLog_logDebug(LogType::Force, "D3D12: Sampler heap full, resetting");
		SubmitCommandList(true);
		m_gpuSamplerHeap->Reset();
	}
	cemu_assert(false); // a single draw can never exceed the heap
}

/* --- render targets --- */

void D3D12Renderer::draw_applyRenderTargets()
{
	LatteProfiler::Scope profileScope(LatteProfiler::Slot::RenderTargets);
	CachedFBOD3D12 *fbo = m_state.activeFBO;
	for (uint32 i = 0; i < 8; i++)
	{
		LatteTextureViewD3D12 *view = fbo->GetColorView(i);
		if (!view || fbo->GetRTVFormat(i) == DXGI_FORMAT_UNKNOWN)
			continue;
		TransitionTexture(view->GetBaseTexture(), view->firstMip, 1, view->firstSlice, view->numSlice, D3D12_RESOURCE_STATE_RENDER_TARGET);
	}
	if (LatteTextureViewD3D12 *depthView = fbo->GetDepthView())
	{
		auto *depth = depthView->GetBaseTexture();
		TransitionTexture(depth, depthView->firstMip, 1, depthView->firstSlice, depthView->numSlice, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		// ponytail: full-size scene attachment heuristic, not recovered game-camera metadata.
		// Shadow maps and smaller auxiliary passes must not replace the scene depth.
		if (m_fsrInputWidth && depth->GetDesc().Width == m_fsrInputWidth && depth->GetDesc().Height == m_fsrInputHeight &&
			depthView->firstMip == 0 && depthView->firstSlice == 0 && fbo->GetColorView(0))
		{
			const auto uses = ++m_sceneDepthUses[depth];
			if (!m_sceneDepth || uses > m_sceneDepthUses[m_sceneDepth])
				m_sceneDepth = depth;
		}
	}
	if (!m_state.renderTargetsDirty)
		return;
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = fbo->GetDSV();
	m_cmdList->OMSetRenderTargets(fbo->GetNumRTVs(), fbo->GetRTVs(), FALSE, fbo->HasDSV() ? &dsv : nullptr);
	m_state.renderTargetsDirty = false;
}

/* --- draw --- */

void D3D12Renderer::draw_execute(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext &drawcallContext)
{
	LatteProfiler::Scope decodeScope(LatteProfiler::Slot::DrawStateDecode);
	LatteGPUState.drawCallCounter++;
	if (m_state.skipDrawSequence)
		return;
	if (LatteGPUState.contextNew.GetSpecialStateValues()[8] != 0)
	{
		LatteDraw_handleSpecialState8_clearAsDepth();
		return;
	}
	if (LatteGPUState.contextNew.GetSpecialStateValues()[5] != 0)
	{
		draw_handleSpecialState5();
		return;
	}

	m_state.verticesPerInstance = count;
	if (m_state.streamoutEnabled)
	{
		LatteProfiler::DetailScope detailScope(LatteProfiler::Detail::Streamout);
		LatteStreamout_PrepareDrawcall(count, instanceCount);
	}

	auto [fetchShader, vertexShader, pixelShader, geometryShader] = LatteSHRC_GetActiveShaders();
	if (!vertexShader || !fetchShader)
		return;

	// indices (decoded and cached by LatteIndices, stored in upload memory)
	const LattePrimitiveMode primitiveMode = static_cast<LattePrimitiveMode>(LatteGPUState.contextRegister[mmVGT_PRIMITIVE_TYPE]);
	Renderer::INDEX_TYPE hostIndexType;
	uint32 hostIndexCount;
	uint32 indexMax = 0;
	Renderer::IndexAllocation indexAllocation;
	LatteIndices_decode(memory_getPointerFromVirtualOffset(indexDataMPTR), indexType, count, primitiveMode, indexMax, hostIndexType, hostIndexCount, indexAllocation);

	// vertex and uniform buffer cache (may record uploads)
	uint8 stageUniformModifiedMask = 0;
	LatteBufferCache_Sync(indexMax + baseVertex, baseInstance, instanceCount, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, stageUniformModifiedMask);

	CachedFBOD3D12 *fbo = m_state.activeFBO;
	if (!fbo)
		return;

	D3D12PipelineInfo *pipeline = m_pipelineCache->GetOrCreate(fetchShader, vertexShader, geometryShader, pixelShader, fbo, LatteGPUState.contextNew, hostIndexType);
	if (!pipeline->isValid())
		return;

	// descriptor content. Sampler tables are acquired first because a full sampler heap forces a submit, which would
	// discard any command list state recorded for this draw
	PreparedStageBindings stageBindings[3];
	uint32 stageBindingCount = 0;
	{
		LatteProfiler::DetailScope detailScope(LatteProfiler::Detail::BindingsPrepare);
		draw_buildStageBindings(vertexShader, static_cast<RendererShaderD3D12 *>(vertexShader->shader), stageBindings[stageBindingCount++]);
		if (pixelShader)
			draw_buildStageBindings(pixelShader, static_cast<RendererShaderD3D12 *>(pixelShader->shader), stageBindings[stageBindingCount++]);
		if (geometryShader)
			draw_buildStageBindings(geometryShader, static_cast<RendererShaderD3D12 *>(geometryShader->shader), stageBindings[stageBindingCount++]);
		draw_acquireSamplerTables(stageBindings, stageBindingCount);
	}
	// Heap exhaustion can submit and retire upload allocations. Prepare transient uniforms afterwards.
	{
		LatteProfiler::Scope profileScope(LatteProfiler::Slot::Uniforms);
		LatteDecompilerShader *shaders[] = {vertexShader, pixelShader, geometryShader};
		for (uint32 i = 0; i < stageBindingCount; ++i)
		{
			auto &bindings = stageBindings[i];
			auto *shader = shaders[(uint32)bindings.stage];
			if (shader->resourceMapping.uniformVarsBufferBindingPoint < 0)
				continue;
			const auto &remap = static_cast<RendererShaderD3D12 *>(shader->shader)->GetBindingRemap();
			const sint32 reg = remap.GetCBV(shader->resourceMapping.uniformVarsBufferBindingPoint);
			if (reg >= 0)
				draw_prepareUniformVars(shader, bindings.tableDesc.cbvAddress[reg], bindings.tableDesc.cbvSize[reg]);
		}
	}

	const bool usesStreamout = vertexShader->resourceMapping.tfStorageBindingPoint >= 0;
	{
		LatteProfiler::Scope profileScope(LatteProfiler::Slot::RenderState);
		// resource states. Inputs first so render targets win if a texture is both sampled and rendered to
		if (vertexShader)
			draw_transitionInputTextures(vertexShader);
		if (pixelShader)
			draw_transitionInputTextures(pixelShader);
		if (geometryShader)
			draw_transitionInputTextures(geometryShader);
		draw_applyRenderTargets();
		TransitionResource(m_bufferCache.Get(), m_bufferCacheState, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_COPY_SOURCE);
		if (usesStreamout)
			TransitionResource(m_xfbRingBuffer.Get(), m_xfbRingBufferState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		FlushBarriers();
	}

	// pipeline state
	if (!m_state.rootSignatureBound)
		BindRootSignatureAndHeaps();

	if (m_state.currentPSO != pipeline->pso.Get() || m_state.currentTopology != pipeline->topology)
	{
		LatteProfiler::Scope profileScope(LatteProfiler::Slot::PipelineBind);
		if (m_state.currentPSO != pipeline->pso.Get())
		{
			m_cmdList->SetPipelineState(pipeline->pso.Get());
			m_state.currentPSO = pipeline->pso.Get();
		}
		if (m_state.currentTopology != pipeline->topology)
		{
			m_cmdList->IASetPrimitiveTopology(pipeline->topology);
			m_state.currentTopology = pipeline->topology;
		}
	}

	if (m_state.vertexBufferDirtyMask)
	{
		LatteProfiler::Scope profileScope(LatteProfiler::Slot::VertexBuffers);
		const uint32 first = std::countr_zero(m_state.vertexBufferDirtyMask);
		const uint32 last = 32 - std::countl_zero(m_state.vertexBufferDirtyMask);
		m_cmdList->IASetVertexBuffers(first, last - first, m_state.vertexBuffers + first);
		m_state.vertexBufferDirtyMask = 0;
	}

	{
		LatteProfiler::Scope profileScope(LatteProfiler::Slot::RenderState);
		if (m_state.viewportDirty)
		{
			m_cmdList->RSSetViewports(1, &m_state.viewport);
			m_state.viewportDirty = false;
		}
		if (m_state.scissorDirty)
		{
			m_cmdList->RSSetScissorRects(1, &m_state.scissor);
			m_state.scissorDirty = false;
		}
		if (pipeline->usesBlendConstants)
		{
			float blendFactor[4];
			memcpy(blendFactor, LatteGPUState.contextRegister + Latte::REGADDR::CB_BLEND_RED, sizeof(blendFactor));
			if (pipeline->blendConstantAlphaOnly)
				blendFactor[0] = blendFactor[1] = blendFactor[2] = blendFactor[3];
			if (memcmp(blendFactor, m_state.currentBlendFactor, sizeof(blendFactor)) != 0)
			{
				m_cmdList->OMSetBlendFactor(blendFactor);
				memcpy(m_state.currentBlendFactor, blendFactor, sizeof(blendFactor));
			}
		}
		if (pipeline->usesStencil)
		{
			const uint32 ref = LatteGPUState.contextNew.DB_STENCILREFMASK.get_STENCILREF_F();
			if (ref != m_state.currentStencilRef)
			{
				m_cmdList->OMSetStencilRef(ref);
				m_state.currentStencilRef = ref;
			}
		}
	}

	const bool isIndexed = hostIndexType != INDEX_TYPE::NONE;
	if (isIndexed)
	{
		LatteProfiler::Scope profileScope(LatteProfiler::Slot::VertexBuffers);
		// index buffer
		auto *allocation = (D3D12UploadHeapAllocator::Allocation *)indexAllocation.rendererInternal;
		if (!allocation)
			return;
		D3D12_INDEX_BUFFER_VIEW ib;
		ib.BufferLocation = allocation->gpuAddress;
		ib.Format = (hostIndexType == INDEX_TYPE::U16) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
		ib.SizeInBytes = allocation->size;
		if (memcmp(&ib, &m_state.currentIndexBuffer, sizeof(ib)) != 0)
		{
			m_cmdList->IASetIndexBuffer(&ib);
			m_state.currentIndexBuffer = ib;
		}
	}

	{
		LatteProfiler::Scope profileScope(LatteProfiler::Slot::Descriptors);
		// descriptor tables
		for (uint32 i = 0; i < stageBindingCount; i++)
		{
			const PreparedStageBindings &b = stageBindings[i];
			BindStageRootCBVs(b.tableDesc, b.stage);
			const auto table = WriteStageTable(b.tableDesc, b.stage);
			auto &currentView = m_state.currentViewTables[(uint32)b.stage];
			auto &currentSampler = m_state.currentSamplerTables[(uint32)b.stage];
			if (currentView.ptr != table.ptr)
			{
				m_cmdList->SetGraphicsRootDescriptorTable(D3D12Const::RootParamStageTable(b.stage), table);
				currentView = table;
			}
			if (currentSampler.ptr != b.samplerTable.ptr)
			{
				m_cmdList->SetGraphicsRootDescriptorTable(D3D12Const::RootParamStageSamplers(b.stage), b.samplerTable);
				currentSampler = b.samplerTable;
			}
		}

		// gl_VertexIndex / gl_InstanceIndex include the base vertex/instance in Vulkan but SV_VertexID / SV_InstanceID
		// don't. The translated shaders add these offsets back, see D3D12ShaderTranslate::SPIRVToHLSL
		uint32 runtimeDwords[D3D12Const::kRuntimeDataDwords]{};
#ifdef CEMU_D3D12_SPIRV_TO_DXIL
		if (D3D12ShaderCompiler::GetBackend() == D3D12ShaderCompiler::Backend::SpirvToDXIL)
		{
			dxil_spirv_vertex_runtime_data runtimeData{};
			runtimeData.first_vertex = baseVertex;
			runtimeData.base_instance = baseInstance;
			runtimeData.is_indexed_draw = isIndexed;
			memcpy(runtimeDwords, &runtimeData, sizeof(runtimeData));
		}
		else
#endif
		{
			runtimeDwords[0] = baseVertex;	 // SPIRV_Cross_BaseVertex
			runtimeDwords[1] = baseInstance; // SPIRV_Cross_BaseInstance
		}
		if (!m_state.runtimeDataValid || memcmp(m_state.currentRuntimeData, runtimeDwords, sizeof(runtimeDwords)) != 0)
		{
			m_cmdList->SetGraphicsRoot32BitConstants(D3D12Const::kRootParamRuntimeData, D3D12Const::kRuntimeDataDwords, runtimeDwords, 0);
			memcpy(m_state.currentRuntimeData, runtimeDwords, sizeof(runtimeDwords));
			m_state.runtimeDataValid = true;
		}
	}

	{
		LatteProfiler::Scope profileScope(LatteProfiler::Slot::DrawCall);
		if (isIndexed)
			m_cmdList->DrawIndexedInstanced(hostIndexCount, instanceCount, 0, (INT)baseVertex, baseInstance);
		else
			m_cmdList->DrawInstanced(count, instanceCount, baseVertex, baseInstance);
	}

	if (usesStreamout)
		UAVBarrier(m_xfbRingBuffer.Get());
	if (m_state.streamoutEnabled)
	{
		LatteProfiler::DetailScope detailScope(LatteProfiler::Detail::Streamout);
		LatteStreamout_FinishDrawcall(false);
	}
	m_drawsInCommandList++;
	m_statDrawsPerFrame++;
	m_hasRecordedWork = true;
}

#endif // ENABLE_D3D12
