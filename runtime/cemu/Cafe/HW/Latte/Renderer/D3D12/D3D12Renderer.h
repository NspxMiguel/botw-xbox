// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Ported from PrismFox67/Cemu-UWP; see runtime/cemu/VENDORED.md.

#pragma once

#include "Cafe/HW/Latte/Renderer/D3D12/LatteBatchPolicy.h"

#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12StageTable.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Memory.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DescriptorHeaps.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Format.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h"
#include "Cafe/HW/Latte/Core/LatteProfiler.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LattePipeline.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12TextureStage.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DrawPacket.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ParallelRecorder.h"
#include "util/math/vector2.h"

#include "Cafe/HW/Latte/Core/LatteDrawCache.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>

class LatteTextureD3D12;
class LatteTextureViewD3D12;
class CachedFBOD3D12;
class RendererShaderD3D12;
class D3D12SwapChain;
class D3D12PipelineCache;
struct D3D12PipelineInfo;
class D3D12ImGuiRenderer;
class D3D12FrameInterpolator;
class D3D12Fsr;
class D3D12Ffx;
class D3D12SimSeriesS;
class LatteQueryObjectD3D12;
struct LatteDecompilerShader;

namespace WindowSystem
{
struct WindowHandleInfo;
}

class D3D12Renderer : public Renderer, public D3D12SubmissionTracker
{
	friend class LatteQueryObjectD3D12;
	friend class LatteTextureReadbackInfoD3D12;
	friend class D3D12ImGuiRenderer;
	friend class LatteTextureViewD3D12;

  public:
	// textures are uploaded through the shared upload ring; anything larger goes to a dedicated buffer
	static constexpr uint64 kUploadRingSize = 128ull * 1024 * 1024;
	static constexpr uint64 kReadbackRingSize = 64ull * 1024 * 1024;
	static constexpr uint32 kOcclusionQueryCount = 4096;
	// submit after this many draws so CPU and GPU work overlaps and the descriptor ring stays bounded
	static constexpr uint32 kDrawsPerSubmission = 2000;

	static D3D12Renderer *GetInstance();

	struct AdapterInfo
	{
		std::string name;
		uint64 luid; // stored in the config as d3d12Adapter
	};
	static std::vector<AdapterInfo> GetAdapters();

	D3D12Renderer();
	~D3D12Renderer() override;

	// --- surface management ---
	// BotWRecomp: the host passes HWND, CoreWindow IUnknown*, or nullptr for composition.
	// Attach composition swap chains with ISwapChainPanelNative::SetSwapChain(GetSwapChain(...)).
	enum class SurfaceType
	{
		HWND,
		CoreWindow,
		Composition
	};
	void InitializeSurface(const Vector2i &size, bool mainWindow, SurfaceType type, void *nativeHandle);
	void ShutdownSurface(bool mainWindow);
	void ResizeSurface(const Vector2i &size, bool mainWindow);
	IDXGISwapChain1 *GetSwapChain(bool mainWindow);

	// --- Renderer interface ---
	void Initialize() override;
	void Shutdown() override;
	bool IsPadWindowActive() override;

	bool GetVRAMInfo(int &usageInMB, int &totalInMB) const override;
	void EnableDebugMode() override;

	void ClearColorbuffer(bool padView) override;
	void DrawEmptyFrame(bool mainWindow) override;
	void SwapBuffers(bool swapTV, bool swapDRC) override;

	void HandleScreenshotRequest(LatteTextureView *texView, bool padView) override;

	void DrawBackbufferQuad(LatteTextureView *texView, RendererOutputShader *shader, bool useLinearTexFilter,
							sint32 imageX, sint32 imageY, sint32 imageWidth, sint32 imageHeight,
							bool padView, bool clearBackground) override;
	bool BeginFrame(bool mainWindow) override;

	void Flush(bool waitIdle = false) override;
	void NotifyLatteCommandProcessorIdle() override;

	bool ImguiBegin(bool mainWindow) override;
	void ImguiEnd() override;
	ImTextureID GenerateTexture(const std::vector<uint8> &data, const Vector2i &size) override;
	void DeleteTexture(ImTextureID id) override;
	void DeleteFontTextures() override;

	bool UseTFViaSSBO() const override
	{
		return true;
	}
	void AppendOverlayDebugInfo() override;

	void renderTarget_setViewport(float x, float y, float width, float height, float nearZ, float farZ, bool halfZ = false) override;
	void renderTarget_setScissor(sint32 scissorX, sint32 scissorY, sint32 scissorWidth, sint32 scissorHeight) override;

	LatteCachedFBO *rendertarget_createCachedFBO(uint64 key) override;
	void rendertarget_deleteCachedFBO(LatteCachedFBO *fbo) override;
	void rendertarget_bindFramebufferObject(LatteCachedFBO *cfbo) override;

	void *texture_acquireTextureUploadBuffer(uint32 size) override;
	void texture_releaseTextureUploadBuffer(uint8 *mem) override;

	TextureDecoder *texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, uint32 width, uint32 height) override;

	void texture_clearSlice(LatteTexture *hostTexture, sint32 sliceIndex, sint32 mipIndex) override;
	void texture_loadSlice(LatteTexture *hostTexture, sint32 width, sint32 height, sint32 depth, void *pixelData, sint32 sliceIndex, sint32 mipIndex, uint32 compressedImageSize) override;
	void texture_clearColorSlice(LatteTexture *hostTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a) override;
	void texture_clearDepthSlice(LatteTexture *hostTexture, uint32 sliceIndex, sint32 mipIndex, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue) override;

	LatteTexture *texture_createTextureEx(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth) override;

	void texture_setLatteTexture(LatteTextureView *textureView, uint32 textureUnit) override;
	void texture_copyImageSubData(LatteTexture *src, sint32 srcMip, sint32 effectiveSrcX, sint32 effectiveSrcY, sint32 srcSlice, LatteTexture *dst, sint32 dstMip, sint32 effectiveDstX, sint32 effectiveDstY, sint32 dstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight, sint32 srcDepth) override;

	LatteTextureReadbackInfo *texture_createReadback(LatteTextureView *textureView) override;

	void surfaceCopy_copySurfaceWithFormatConversion(LatteTexture *sourceTexture, sint32 srcMip, sint32 srcSlice, LatteTexture *destinationTexture, sint32 dstMip, sint32 dstSlice, sint32 width, sint32 height) override;

	void bufferCache_init(const sint32 bufferSize) override;
	void bufferCache_upload(uint8 *buffer, sint32 size, uint32 bufferOffset) override;
	void bufferCache_copy(uint32 srcOffset, uint32 dstOffset, uint32 size) override;
	void bufferCache_copyStreamoutToMainBuffer(uint32 srcOffset, uint32 dstOffset, uint32 size) override;

	void buffer_bindVertexBuffers(std::span<BindBufferParam> bindings) override;
	void buffer_bindUniformBuffer(LatteConst::ShaderType shaderType, uint32 bufferIndex, uint32 offset, uint32 size) override;

	RendererShader *shader_create(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, const std::string &source, bool compileAsync, bool isGfxPackSource) override;

	void streamout_setupXfbBuffer(uint32 bufferIndex, sint32 ringBufferOffset, uint32 rangeAddr, uint32 rangeSize) override;
	void streamout_begin() override;
	void streamout_rendererFinishDrawcall() override;

	void draw_beginSequence() override;
	void draw_execute(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext &drawcallContext) override;
	void draw_endSequence() override;

	IndexAllocation indexData_reserveIndexMemory(uint32 size) override;
	void indexData_releaseIndexMemory(IndexAllocation &allocation) override;
	void indexData_uploadIndexMemory(IndexAllocation &allocation) override;

	LatteQueryObject *occlusionQuery_create() override;
	void occlusionQuery_destroy(LatteQueryObject *queryObj) override;
	void occlusionQuery_flush() override;
	void occlusionQuery_updateState() override;

	// --- D3D12SubmissionTracker ---
	uint64 GetRecordingSubmissionId() const override
	{
		m_pipeline.CheckBack();
		return m_lastSubmittedFenceValue + 1;
	}
	uint64 GetCompletedSubmissionId() override;
	void WaitForSubmission(uint64 submissionId) override;

	// --- internal API used by the other backend objects ---
	ID3D12Device *GetDevice() const
	{
		return m_device.Get();
	}
	ID3D12GraphicsCommandList *GetCommandList() const
	{
		m_pipeline.CheckBack();
		return m_cmdList.Get();
	}
	const D3D12FormatSupport &GetFormatSupport() const
	{
		return m_formatSupport;
	}
	bool IsDebugNamingEnabled() const
	{
		return m_debugMode;
	}
	LattePipeline::ResourceId RegisterResource(void *object)
	{
		m_pipeline.CheckFront();
		return m_resources.Insert(object);
	}
	void InitializeTexture(LattePipeline::ResourceId id);
	void InitializeFramebuffer(LattePipeline::ResourceId id);
	void ReleaseTexture(LattePipeline::ResourceId id);
	void ReleaseView(LattePipeline::ResourceId id);
	void ReleaseFramebuffer(LattePipeline::ResourceId id);
	void RetireResource(LattePipeline::ResourceId id);
	void WaitForRecording();
	uint64 GenUniqueId()
	{
		return ++m_uniqueIdCounter;
	}

	D3D12StagingDescriptorHeap &GetStagingViewHeap()
	{
		m_pipeline.CheckBack();
		return *m_stagingViewHeap;
	}
	D3D12StagingDescriptorHeap &GetStagingRTVHeap()
	{
		m_pipeline.CheckBack();
		return *m_stagingRTVHeap;
	}
	D3D12StagingDescriptorHeap &GetStagingDSVHeap()
	{
		m_pipeline.CheckBack();
		return *m_stagingDSVHeap;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE GetNullRTV() const
	{
		return m_nullRTV;
	}
	ID3D12RootSignature *GetRootSignature() const
	{
		return m_rootSignature.Get();
	}
	D3D12PipelineCache *GetPipelineCache() const
	{
		return m_pipelineCache.get();
	}
	bool IsPipelineLibrarySupported() const
	{
		return m_pipelineLibrarySupported;
	}
	bool IsPipelineBlobCacheSupported() const
	{
		return m_pipelineBlobsSupported;
	}

	D3D12UploadAllocation AllocateUpload(uint64 size, uint64 alignment)
	{
		m_pipeline.CheckBack();
		LatteProfiler::Add(LatteProfiler::Counter::UploadBytes, size);
		return m_uploadRing->Allocate(size, alignment);
	}
	void ReleaseResourceDeferred(ComPtr<ID3D12Resource> resource);
	void ReleaseObjectDeferred(ComPtr<IUnknown> object);

	// called from destructors so cached references can be dropped
	void NotifyTextureRelease(LatteTextureD3D12 *texture);
	void NotifyTextureViewRelease(LatteTextureViewD3D12 *view);
	void NotifyFBORelease(CachedFBOD3D12 *fbo);

	// state tracking. Barriers are batched and flushed before the next command that needs them
	void TransitionTexture(LatteTextureD3D12 *texture, uint32 firstMip, uint32 mipCount, uint32 firstSlice, uint32 sliceCount, D3D12_RESOURCE_STATES newState)
	{
		m_pipeline.CheckBack();
		if (!texture->GetResource())
			return;
		constexpr auto writeStates = D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_RESOLVE_DEST;
		// This must run even on hits: copied slice SRVs track content, not barriers.
		if (newState & writeStates)
			texture->MarkContentWritten();
		if (texture->Is3DTexture())
		{
			firstSlice = 0;
			sliceCount = 1;
		}
		const LatteTextureD3D12::TransitionKey key{firstMip, mipCount, firstSlice, sliceCount, (uint32)newState};
		if (texture->ReuseTransition(key))
		{
			LatteProfiler::Add(LatteProfiler::Counter::TransitionHits);
			return;
		}
		TransitionTextureSlow(texture, firstMip, mipCount, firstSlice, sliceCount, newState);
	}
	void TransitionTextureSlow(LatteTextureD3D12 *texture, uint32 firstMip, uint32 mipCount, uint32 firstSlice, uint32 sliceCount, D3D12_RESOURCE_STATES newState);
	void TransitionTextureAll(LatteTextureD3D12 *texture, D3D12_RESOURCE_STATES newState);
	void TransitionResource(ID3D12Resource *resource, D3D12_RESOURCE_STATES &trackedState, D3D12_RESOURCE_STATES newState)
	{
		m_pipeline.CheckBack();
		if (trackedState == newState)
			return;
		m_pendingBarriers.push_back(D3D12_TransitionBarrier(resource, trackedState, newState));
		trackedState = newState;
	}
	void UAVBarrier(ID3D12Resource *resource);
	void FlushBarriers();

	void SubmitCommandList(bool waitIdle = false);
	void WaitForIdle();
	void RequestSubmitSoon()
	{
		m_pipeline.CheckBack();
		m_submitSoon = true;
	}

	[[noreturn]] void HandleDeviceError(HRESULT hr, const char *what);
	void LogDeviceRemovedData(); // DRED breadcrumbs and page fault info, logged once

	// used by internal shaders (surface copy, output blit, imgui)
	RendererShaderD3D12 *CreateInternalShader(RendererShader::ShaderType type, const std::string &glsl);
	void BindRootSignatureAndHeaps();
	void SetPushConstants(const void *data, uint32 sizeInBytes);
	D3D12_GPU_DESCRIPTOR_HANDLE WriteStageTable(const D3D12StageTableDesc &tableDesc, D3D12Const::Stage stage = D3D12Const::Stage::Count);
	// May submit the command list and reset the shader-visible sampler heap if it is full. All command list state is
	// lost when that happens, so call this before recording any state that the following draw depends on.
	D3D12_GPU_DESCRIPTOR_HANDLE GetSamplerTable(const D3D12_SAMPLER_DESC *samplers, uint32 count);
	void BindStageRootCBVs(const D3D12StageTableDesc &bindings, D3D12Const::Stage stage);
	void WriteCBV(D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_GPU_VIRTUAL_ADDRESS address, uint32 size);
	void InvalidateDrawState(); // forces all cached command list state to be re-applied

	D3D12SwapChain *GetMainSwapChain()
	{
		return m_swapChainMain.get();
	}
	D3D12Ffx *GetFfx()
	{
		return m_ffx.get();
	}
	LatteTextureD3D12 *GetSceneDepth()
	{
		return m_sceneDepth;
	}
	ID3D12CommandQueue *GetQueue()
	{
		return m_queue.Get();
	}

  private:
	LattePipeline::Context m_pipeline;
	LattePipeline::Stream m_operations{
		LattePipeline::CapacityFromEnvironment("CEMU_LATTE_PIPELINE_CAPACITY", 1024, 1, 65536),
		LattePipeline::CapacityFromEnvironment("CEMU_LATTE_PIPELINE_ARENA", 16 * 1024 * 1024, 1024 * 1024, 64 * 1024 * 1024)};
	LattePipeline::Registry m_resources;
	LattePipeline::ResourceId m_recordingFBO;
	LattePipeline::BlockId m_pinnedDrawBlock;
	LattePipeline::ResourceId m_pinnedFramebuffer;
	uint64 m_pinnedDrawFence = 0;
	LattePipeline::StateBlocks<LattePipeline::DrawBlock, 1024, LattePipeline::DrawBlockEqual, LattePipeline::DrawBlockCopy> m_drawBlocks;
	LattePipeline::SequenceStageLayouts m_frontStageLayouts;
	LattePipeline::DrawBlock m_frontDrawBlock{};
	LattePipeline::BlockId m_frontDrawBlockId;
	bool m_frontDrawBlockReusable = false;
	std::array<LatteDecompilerShader *, 3> m_frontDrawShaders{};
	struct IndexResource
	{
		D3D12UploadHeapAllocator::Allocation *native = nullptr;
		std::shared_ptr<LattePipeline::DeferredIndex> deferred;
		uint32 size = 0;
		LattePipeline::ResourceId id;
	};
	struct FrontState
	{
		bool skipDrawSequence = false, streamoutEnabled = false;
		uint32 verticesPerInstance = 0;
		struct
		{
			bool enabled = false;
			uint32 ringBufferOffset = 0;
		} streamoutBuffer[LATTE_NUM_STREAMOUT_BUFFER];
		LatteTextureViewD3D12 *boundTexture[LATTE_CEMU_GS_TEX_UNIT_BASE + LATTE_NUM_MAX_TEX_UNITS]{};
		CachedFBOD3D12 *activeFBO = nullptr;
		D3D12_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
		D3D12_RECT scissor{0, 0, 1, 1};
	} m_frontState;
	struct FrontBindings
	{
		LattePipeline::VertexRange vertices[LATTE_MAX_VERTEX_BUFFERS]{};
		LattePipeline::UniformRange uniforms[3][LATTE_NUM_MAX_UNIFORM_BUFFERS]{};
	} m_frontBindings;
	std::vector<uint8> m_indexDecodeScratch;
	std::vector<std::byte> m_indexCaptureScratch;
	bool indexData_captureIndexMemory(const void *data, uint32 size, uint32 width, bool bigEndian, IndexAllocation &allocation, uint32 &maximum) override;
	LattePipeline::Span StageBytes(const void *data, size_t size);
	void draw_executeDirect(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext &drawcallContext);
	void draw_prepareStage(LatteDecompilerShader *shader, LattePipeline::StageBlock &out);
	void draw_record(const LattePipeline::DrawPacket &packet, const LattePipeline::Arena &arena);
	bool draw_captureParallel(const LattePipeline::DrawPacket &packet, const LattePipeline::Arena &arena);
	void draw_drainParallel();
	void draw_planRenderTargets(CachedFBOD3D12 *fbo);
	bool HasActiveOcclusionQuery() const;
	std::unique_ptr<D3D12ParallelRecorder> m_parallelRecorder;
	std::vector<D3D12ResolvedDraw> m_parallelDraws;
	bool m_parallelReady = false;
	size_t m_parallelBytes = 0;
	LatteProfiler::Clock::time_point m_parallelBatchStart{};
	const unsigned m_requestedChunks = static_cast<unsigned>(LattePipeline::CapacityFromEnvironment("CEMU_LATTE_CHUNKS", 0, 1, 4));
	void draw_recordUniformBytes(D3D12Const::Stage stage, std::span<const std::byte> bytes, D3D12_GPU_VIRTUAL_ADDRESS &addressOut, uint32 &sizeOut);
	std::span<const std::byte> draw_buildUniformVars(LatteDecompilerShader *shader);
	LattePipeline::Span draw_snapshotUniformVars(LatteDecompilerShader *shader);

	struct ReadbackPacket
	{
		LattePipeline::ResourceId readback, texture;
		uint32 action = 0, mip = 0, slice = 0, rowPitch = 0, imageSize = 0, width = 0, height = 0;
	};
	void readback_record(const ReadbackPacket &packet, const LattePipeline::Arena &arena);
	struct QueryPacket
	{
		LattePipeline::ResourceId query;
		uint32 action = 0;
		uint64 epoch = 0;
	};
	void query_record(const QueryPacket &packet, const LattePipeline::Arena &arena);
	void occlusionQuery_createHeap_record();
	struct OutputPacket
	{
		LattePipeline::ResourceId view, vertexShader, pixelShader;
		RendererOutputShader::OutputUniformVariables uniforms[2];
		sint32 imageX, imageY, imageWidth, imageHeight, inputWidth, inputHeight;
		uint32 upscaler;
		bool linear, padView, clearBackground, nativeAA;
	};
	static_assert(sizeof(OutputPacket) <= 208);
	void output_record(const OutputPacket &packet, const LattePipeline::Arena &arena);
	struct BackendPacket
	{
		using Kind = LattePipeline::BackendKind;
		Kind kind{};
		LattePipeline::ResourceId resources[2]{};
		uint32 words[24]{};
		LattePipeline::Span bytes;
	};
	static_assert(sizeof(BackendPacket) <= 208);
	void buffer_bindVertexBuffers_record(std::span<const BindBufferParam> bindings);
	void buffer_bindUniformBuffer_record(uint32 stage, uint32 bufferIndex, uint32 offset, uint32 size);
	void renderTarget_setViewport_record(D3D12_VIEWPORT viewport);
	void renderTarget_setScissor_record(D3D12_RECT scissor);
	void bufferCache_upload_record(const void *buffer, uint32 size, uint32 bufferOffset);
	void bufferCache_init_record(uint32 bufferSize, uint32 streamoutSize);

	void surfaceCopy_record(const BackendPacket &packet);
	void texture_recordPlane(const BackendPacket &packet, const LattePipeline::Arena &arena);
	void texture_preparePlane(LatteTextureD3D12 *texture, uint32 mip, uint32 slice, uint32 plane, const uint8 *data, uint32 rowBytes, uint32 rows, uint32 width, uint32 blockDim, bool first);
	void ApplyFrontCompletions();
	void UpdatePipelineProfile();
	enum CompletionKind : uint32
	{
		QueryResult = 1,
		ReadbackResult,
		ProfileResult,
		QueryBatchResult
	};
	void AcceptCompletion(const LattePipeline::Completion &completion);
	void WorkerIdle();
	std::unordered_map<uint64, LatteQueryObjectD3D12 *> m_frontQueries;
	std::vector<LatteQueryObjectD3D12 *> m_cachedQueries;
	bool m_frontQueryHeapReady = false;
	std::unordered_map<uint64, LatteTextureReadbackInfoD3D12 *> m_frontReadbacks;
	static uint64 ResourceKey(LattePipeline::ResourceId id)
	{
		return (uint64(id.generation) << 32) | id.slot;
	}
	void AcceptQueryBatch(const LattePipeline::Completion &completion);
	void AcceptQueryResult(const LattePipeline::Completion &completion);
	void AcceptReadbackResult(const LattePipeline::Completion &completion);
	bool m_applyingCompletions = false, m_frontReadbacksReady = false;
	uint64 m_frontCompleted = 0;
	LattePipeline::PipelineTiming m_profilePipelineTiming;
	uint64 m_frontCpuStart = 0, m_backIdleObserved = 0;
	double m_reportSeconds = 0;
	uint64 m_reportFrames = 0;
	std::vector<uint8> m_screenshotBytes;
	struct UISnapshot;

	void *CreateUITexture(const uint8 *rgba, sint32 width, sint32 height);
	void ui_record(const BackendPacket &packet);

	void capture_record(const BackendPacket &packet);
	bool BeginFrame_record(bool mainWindow);
	std::deque<std::pair<uint64, uint64>> m_submissionSequences;
	int m_presentVsync = 1;
	bool m_presentInterpolation = false;
	void RecordBackend(const BackendPacket &packet, const LattePipeline::Arena &arena);
	LattePipeline::Completion ExecuteBackend(LattePipeline::Opcode opcode, const BackendPacket &packet);
	void texture_clearColorSlice_record(LatteTexture *hostTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a);
	void texture_clearDepthSlice_record(LatteTexture *hostTexture, uint32 sliceIndex, sint32 mipIndex, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue);
	void texture_copyImageSubData_record(LatteTexture *src, sint32 srcMip, sint32 effectiveSrcX, sint32 effectiveSrcY, sint32 srcSlice, LatteTexture *dst, sint32 dstMip, sint32 effectiveDstX, sint32 effectiveDstY, sint32 dstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight, sint32 srcDepth);
	void bufferCache_copy_record(uint32 srcOffset, uint32 dstOffset, uint32 size);
	void bufferCache_copyStreamoutToMainBuffer_record(uint32 srcOffset, uint32 dstOffset, uint32 size);
	void streamout_rendererFinishDrawcall_record();
	void SubmitCommandList_record(bool waitIdle);
	void Flush_record(bool waitIdle);
	void NotifyLatteCommandProcessorIdle_record();
	void SwapBuffers_record(bool swapTV, bool swapDRC);
	void ClearColorbuffer_record(bool padView);
	void DrawEmptyFrame_record(bool mainWindow);
	void occlusionQuery_flush_record();
	void occlusionQuery_updateState_record();

	struct DrawState;
	struct StageBindings;

	void CreateDevice();
	void CreateRootSignature();
	void CreateNullDescriptors();
	void ResetCommandList();
	void ProcessFinishedSubmissions();

	D3D12SwapChain *GetSwapChainObj(bool mainWindow)
	{
		return mainWindow ? m_swapChainMain.get() : m_swapChainPad.get();
	}
	bool AcquireBackbuffer(bool mainWindow);
	void PresentSwapChain(bool mainWindow);
	bool PresentInterpolated(D3D12SwapChain *swapChain);
	void UpdatePresentDiagnostics(D3D12SwapChain *swapChain, bool interpolationWanted);
	void DumpTextureBMP(ID3D12Resource *texture, D3D12_RESOURCE_STATES &state, const std::string &path);

	// draw path (D3D12RendererCore.cpp)
	void draw_handleSpecialState5();
	bool draw_prepareUniformVars(LatteDecompilerShader *shader, D3D12_GPU_VIRTUAL_ADDRESS &addressOut, uint32 &sizeOut);
	struct PreparedStageBindings
	{
		D3D12Const::Stage stage;
		D3D12StageTableDesc tableDesc;
		D3D12_SAMPLER_DESC samplers[D3D12Const::kMaxSamplersPerStage];
		uint32 samplerCount;
		D3D12_GPU_DESCRIPTOR_HANDLE samplerTable;
		const D3D12_SAMPLER_DESC *samplerSource = nullptr;
		uint64 samplerIdentity = 0;
		const D3D12_SAMPLER_DESC *SamplerData() const
		{
			return samplerSource ? samplerSource : samplers;
		}
	};
	void draw_buildStageBindings(LatteDecompilerShader *shader, RendererShaderD3D12 *hostShader, PreparedStageBindings &out);
	void draw_acquireSamplerTables(PreparedStageBindings *stages, uint32 count);
	bool TryGetSamplerTable(const D3D12_SAMPLER_DESC *samplers, uint32 count, D3D12_GPU_DESCRIPTOR_HANDLE &tableOut);
	void draw_buildSamplerDesc(LatteDecompilerShader *shader, uint32 relativeTextureUnit, LatteTextureViewD3D12 *view, D3D12_SAMPLER_DESC &desc);
	void draw_applyRenderTargets();
	void draw_transitionInputTextures(LatteDecompilerShader *shader);
	D3D12_CPU_DESCRIPTOR_HANDLE GetNullSRV(Latte::E_DIM dim) const;
	D3D12_GPU_VIRTUAL_ADDRESS GetAlignedUniformBufferAddress(uint32 cacheOffset, uint32 size);

	// surface copy (D3D12SurfaceCopy.cpp)
	void surfaceCopy_init();
	void surfaceCopy_shutdown();
	void surfaceCopy_notifyViewRelease(LatteTextureViewD3D12 *view);

	void occlusionQuery_notifyEndCommandList();
	void occlusionQuery_notifyBeginCommandList();
	void occlusionQuery_destroyAll();
	void ProcessPadSwapChainShutdown();

	// device objects
	ComPtr<IDXGIFactory4> m_dxgiFactory;
	ComPtr<IDXGIAdapter1> m_adapter;
	ComPtr<ID3D12Device> m_device;
	ComPtr<ID3D12CommandQueue> m_queue;
	ComPtr<ID3D12Fence> m_fence;
	HANDLE m_fenceEvent = nullptr;
	uint64 m_lastSubmittedFenceValue = 0;
	uint64 m_completedFenceValue = 0;
	bool m_debugMode = false;
	bool m_dredEnabled = false;
	bool m_dredLogged = false;
	bool m_pipelineLibrarySupported = false;
	bool m_pipelineBlobsSupported = false;
	void TryEnableImplicitShaderCache();
	bool m_implicitCacheTesting = false;
	fs::path m_implicitCacheMarker;
	uint32 m_implicitCacheTestFrames = 0;
	uint64 m_uniqueIdCounter = 0;
	D3D12FormatSupport m_formatSupport;
	D3D_SHADER_MODEL m_highestShaderModel = D3D_SHADER_MODEL_5_1;
	D3D12_RESOURCE_BINDING_TIER m_bindingTier = D3D12_RESOURCE_BINDING_TIER_1;

	std::unique_ptr<D3D12SimSeriesS> m_simSeriesS;

	// command recording
	struct CommandAllocatorEntry
	{
		ComPtr<ID3D12CommandAllocator> allocator;
		uint64 fenceValue;
	};
	std::deque<CommandAllocatorEntry> m_freeAllocators;
	std::deque<CommandAllocatorEntry> m_inFlightAllocators;
	ComPtr<ID3D12CommandAllocator> m_currentAllocator;
	ComPtr<ID3D12GraphicsCommandList> m_cmdList;
	std::vector<D3D12_RESOURCE_BARRIER> m_pendingBarriers;
	uint32 m_drawsInCommandList = 0;
	bool m_submitSoon = false;
	bool m_hasRecordedWork = false;

	// root signature + heaps
	ComPtr<ID3D12RootSignature> m_rootSignature;
	std::unique_ptr<D3D12StagingDescriptorHeap> m_stagingViewHeap;
	std::unique_ptr<D3D12StagingDescriptorHeap> m_stagingSamplerHeap;
	std::unique_ptr<D3D12StagingDescriptorHeap> m_stagingRTVHeap;
	std::unique_ptr<D3D12StagingDescriptorHeap> m_stagingDSVHeap;
	std::unique_ptr<D3D12GpuViewHeap> m_gpuViewHeap;
	std::unique_ptr<D3D12GpuSamplerHeap> m_gpuSamplerHeap;
	std::unique_ptr<D3D12SamplerCache> m_samplerCache;
	D3D12_CPU_DESCRIPTOR_HANDLE m_nullRTV{};
	D3D12_CPU_DESCRIPTOR_HANDLE m_nullCBV{};
	ComPtr<ID3D12Resource> m_zeroConstantBuffer;
	D3D12_GPU_VIRTUAL_ADDRESS m_zeroConstantBufferAddress = 0;
	D3D12_CPU_DESCRIPTOR_HANDLE m_nullUAV{};
	D3D12_CPU_DESCRIPTOR_HANDLE m_nullSRV1D{}, m_nullSRV2D{}, m_nullSRV2DArray{}, m_nullSRVCubeArray{}, m_nullSRV3D{};

	// memory
	std::unique_ptr<D3D12UploadRing> m_uploadRing;
	std::unique_ptr<D3D12UploadHeapAllocator> m_indexAllocator;
	std::unique_ptr<D3D12ReadbackRing> m_readbackRing;
	D3D12DeferredReleaser m_deferredReleaser;

	ComPtr<ID3D12Resource> m_bufferCache;
	D3D12_GPU_VIRTUAL_ADDRESS m_bufferCacheAddress = 0;
	D3D12_RESOURCE_STATES m_bufferCacheState = D3D12_RESOURCE_STATE_COMMON;
	uint32 m_bufferCacheSize = 0;
	ComPtr<ID3D12Resource> m_bufferCacheScratch; // used for overlapping copies and unaligned uniform buffers
	D3D12_RESOURCE_STATES m_bufferCacheScratchState = D3D12_RESOURCE_STATE_COMMON;
	uint32 m_bufferCacheScratchSize = 0;
	uint32 m_bufferCacheScratchOffset = 0;
	ComPtr<ID3D12Resource> m_xfbRingBuffer;
	D3D12_RESOURCE_STATES m_xfbRingBufferState = D3D12_RESOURCE_STATE_COMMON;
	uint32 m_xfbRingBufferSize = 0;

	// texture upload staging memory handed to the texture loader
	std::vector<uint8> m_textureUploadBuffer;

	// swap chains
	std::unique_ptr<D3D12SwapChain> m_swapChainMain;
	std::unique_ptr<D3D12SwapChain> m_swapChainPad;
	std::unique_ptr<D3D12Fsr> m_fsr;
	std::unique_ptr<D3D12Ffx> m_ffx;
	LatteTextureD3D12 *m_sceneDepth = nullptr;
	std::unordered_map<LatteTextureD3D12 *, uint32> m_sceneDepthUses;
	bool m_fsrFailed = false;
	bool m_fsrResizePending = false;
	uint32 m_fsrInputWidth = 0, m_fsrInputHeight = 0, m_fsrOutputWidth = 0, m_fsrOutputHeight = 0;
	std::unique_ptr<D3D12FrameInterpolator> m_frameInterpolator; // created when frame interpolation is first used
	// interpolation presents twice per game frame, which halves the game's speed on a display under 100 Hz: measured every
	// 2 s from the main swap chain (UpdatePresentDiagnostics), interpolation is paused while the display is that slow
	double m_fenceWaitMs = 0;
	// Present diagnostics and Latte profiling share this complete 10 s window.
	std::chrono::steady_clock::time_point m_presentLogStart{};
	uint64 m_presentLogFrames = 0;
	uint64 m_presentLogPresents = 0;
	double m_presentLogBlockedMs = 0;
	double m_presentLogMaxBlockedMs = 0;
	double m_presentLogFenceWaitMs = 0;
	uint64 m_presentLogIdleNs = 0;
	uint64 m_latteCpuTime = 0;
	bool m_interpolationPaused = false;
	// Xbox: vsynced presents run at 60 Hz while the HDMI mode is 120 Hz. Then both frames are presented without vsync and
	// paced by Cemu, half a game frame apart (relies on VRR or tolerates tearing)
	bool m_interpolationUnsynced = false;
	std::chrono::steady_clock::time_point m_lastGameFramePresent{};
	double m_gameFrameIntervalMs = 1000.0 / 60.0;
	// the real frame's present while unsynced, due half a game frame after the halfway frame's (ServicePendingPresent)
	D3D12SwapChain *m_pendingPresentSwapChain = nullptr;
	std::chrono::steady_clock::time_point m_pendingPresentAt{};
	void ServicePendingPresent(bool force);
	std::chrono::steady_clock::time_point m_presentWindowStart{};
	uint32 m_presentLogCounter = 0;
	// the pad window is closed from the UI thread while the render thread may still be using its swap chain
	std::atomic<bool> m_padShutdownRequested{false};
	std::mutex m_padSwapChainMutex;
	std::atomic<uint64> m_surfaceResize[2]{};
	std::atomic<bool> m_padActive{false};

	// pipelines
	std::unique_ptr<D3D12PipelineCache> m_pipelineCache;

	// draw state
	struct StageBindings
	{
		D3D12_GPU_VIRTUAL_ADDRESS uniformBufferAddress[LATTE_NUM_MAX_UNIFORM_BUFFERS]{};
		uint32 uniformBufferSize[LATTE_NUM_MAX_UNIFORM_BUFFERS]{};
	};
	struct DrawState
	{
		CachedFBOD3D12 *activeFBO = nullptr;
		bool renderTargetsDirty = true;
		D3D12_VERTEX_BUFFER_VIEW vertexBuffers[LATTE_MAX_VERTEX_BUFFERS]{};
		uint32 vertexBufferDirtyMask = 0xFFFF;
		StageBindings stage[3];
		D3D12_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
		D3D12_RECT scissor{0, 0, 1, 1};
		bool viewportDirty = true;
		bool scissorDirty = true;
		ID3D12PipelineState *currentPSO = nullptr;
		D3D_PRIMITIVE_TOPOLOGY currentTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
		D3D12_INDEX_BUFFER_VIEW currentIndexBuffer{};
		float currentBlendFactor[4]{-1, -1, -1, -1};
		uint32 currentStencilRef = 0xFFFFFFFF;
		D3D12_GPU_DESCRIPTOR_HANDLE currentViewTables[3]{};
		D3D12_GPU_VIRTUAL_ADDRESS currentRootCBVs[3][D3D12Const::kRootCBVsPerStage]{};
		D3D12_GPU_DESCRIPTOR_HANDLE currentSamplerTables[3]{};
		uint32 currentRuntimeData[D3D12Const::kRuntimeDataDwords]{};
		bool runtimeDataValid = false;
		bool rootSignatureBound = false;

	} m_state;

	// Upload addresses and view tables never escape the submission that owns their ring allocations.
	struct UniformCacheEntry
	{
		// Keep a CPU shadow: mapped upload memory can be write-combined and expensive to read.
		std::vector<uint8> data;
		D3D12_GPU_VIRTUAL_ADDRESS address = 0;
		uint64 submission = ~0ull;
	};
	UniformCacheEntry m_uniformCache[3];
	LatteDrawCache::ExactCache<D3D12StageTableDesc, D3D12_GPU_DESCRIPTOR_HANDLE> m_lastViewTable[4];
	D3D12StageTableCache<> m_viewTableCache[4];
	struct CBVCacheEntry
	{
		D3D12_CPU_DESCRIPTOR_HANDLE descriptor{};
		D3D12_GPU_VIRTUAL_ADDRESS address = 0;
		uint32 size = 0;
	};
	// One staging slot per register avoids source aliasing during the bulk copy. The last bank is for internal passes.
	CBVCacheEntry m_cbvCache[(uint32)D3D12Const::Stage::Count + 1][D3D12Const::kMaxCBVsPerStage];
	struct UAVCacheEntry
	{
		D3D12_CPU_DESCRIPTOR_HANDLE descriptor{};
		ID3D12Resource *resource = nullptr;
		uint32 size = 0;
	};
	UAVCacheEntry m_stageUAVCache[(uint32)D3D12Const::Stage::Count + 1];
	D3D12SamplerTableMemo m_stageSamplerCache[3];
	struct SamplerDescCacheEntry
	{
		std::array<uint32, 13> key{};
		D3D12_SAMPLER_DESC desc{};
		bool valid = false;
	};
	SamplerDescCacheEntry m_samplerDescCache[3][LATTE_NUM_MAX_TEX_UNITS];

	PreparedStageBindings m_preparedStages[3];
	D3D12TextureStageCache m_textureStageCache[3];
	std::vector<uint8> m_uniformScratch; // CPU side staging for uniform var blocks

	// imgui
	std::unique_ptr<D3D12ImGuiRenderer> m_imgui;
	bool m_imguiActive = false;
	bool m_imguiMainWindow = true;

	// occlusion queries
	struct
	{
		ComPtr<ID3D12QueryHeap> heap;
		ComPtr<ID3D12Resource> resultBuffer; // readback heap, one uint64 per query
		uint64 *results = nullptr;
		std::vector<uint32> freeIndices;
		std::vector<LatteQueryObjectD3D12 *> active;

		uint64 lastSubmission = 0;
	} m_occlusionQueries;

	// surface copy / output blit
	struct InternalPipelines;
	std::unique_ptr<InternalPipelines> m_internal;

	// stats
	uint32 m_statDrawsPerFrame = 0;
	uint32 m_statSubmitsPerFrame = 0;
	uint32 m_statBarriersPerFrame = 0;
};
