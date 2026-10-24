#pragma once

#include <cstdint>

namespace LattePipeline
{
enum class BackendKind : uint32_t
{
	texture_clearColorSlice,
	texture_clearDepthSlice,
	texture_copyImageSubData,
	bufferCache_copy,
	bufferCache_copyStreamoutToMainBuffer,
	streamout_rendererFinishDrawcall,
	SubmitCommandList,
	Flush,
	NotifyLatteCommandProcessorIdle,
	SwapBuffers,
	ClearColorbuffer,
	DrawEmptyFrame,
	occlusionQuery_flush,
	occlusionQuery_updateState,
	RegistryRetire,
	UICreate,
	UIUpload,
	UIRelease,
	UIRender,
	UIRenderOwned,
	NativeReleaseOwned,
	UIAcquire,
	Screenshot,
	EndSequence,
	ServicePresent,
	BeginFrame,
	ShutdownDrain,
	RecordingDrain,
	QueryHeap,
	BindVertices,
	BindUniform,
	BindFramebuffer,
	Viewport,
	Scissor,
	TextureCreate,
	FramebufferCreate,
	TextureRelease,
	ViewRelease,
	FramebufferRelease,
	SurfaceCopy,
	BufferUpload,
	IndexCreate,
	IndexDeferredCreate,
	IndexUpload,
	IndexRelease,
	TexturePlane,
	BufferInit,
	DebugSnapshot,
	NativeRelease,
	Count,
};

inline const char *BackendKindName(BackendKind kind)
{
	switch (kind)
	{
	case BackendKind::texture_clearColorSlice:
		return "texture_clearColorSlice";
	case BackendKind::texture_clearDepthSlice:
		return "texture_clearDepthSlice";
	case BackendKind::texture_copyImageSubData:
		return "texture_copyImageSubData";
	case BackendKind::bufferCache_copy:
		return "bufferCache_copy";
	case BackendKind::bufferCache_copyStreamoutToMainBuffer:
		return "bufferCache_copyStreamoutToMainBuffer";
	case BackendKind::streamout_rendererFinishDrawcall:
		return "streamout_rendererFinishDrawcall";
	case BackendKind::SubmitCommandList:
		return "SubmitCommandList";
	case BackendKind::Flush:
		return "Flush";
	case BackendKind::NotifyLatteCommandProcessorIdle:
		return "NotifyLatteCommandProcessorIdle";
	case BackendKind::SwapBuffers:
		return "SwapBuffers";
	case BackendKind::ClearColorbuffer:
		return "ClearColorbuffer";
	case BackendKind::DrawEmptyFrame:
		return "DrawEmptyFrame";
	case BackendKind::occlusionQuery_flush:
		return "occlusionQuery_flush";
	case BackendKind::occlusionQuery_updateState:
		return "occlusionQuery_updateState";
	case BackendKind::RegistryRetire:
		return "RegistryRetire";
	case BackendKind::UICreate:
		return "UICreate";
	case BackendKind::UIUpload:
		return "UIUpload";
	case BackendKind::UIRelease:
		return "UIRelease";
	case BackendKind::UIRender:
		return "UIRender";
	case BackendKind::UIRenderOwned:
		return "UIRenderOwned";
	case BackendKind::NativeReleaseOwned:
		return "NativeReleaseOwned";
	case BackendKind::UIAcquire:
		return "UIAcquire";
	case BackendKind::Screenshot:
		return "Screenshot";
	case BackendKind::EndSequence:
		return "EndSequence";
	case BackendKind::ServicePresent:
		return "ServicePresent";
	case BackendKind::BeginFrame:
		return "BeginFrame";
	case BackendKind::ShutdownDrain:
		return "ShutdownDrain";
	case BackendKind::RecordingDrain:
		return "RecordingDrain";
	case BackendKind::QueryHeap:
		return "QueryHeap";
	case BackendKind::BindVertices:
		return "BindVertices";
	case BackendKind::BindUniform:
		return "BindUniform";
	case BackendKind::BindFramebuffer:
		return "BindFramebuffer";
	case BackendKind::Viewport:
		return "Viewport";
	case BackendKind::Scissor:
		return "Scissor";
	case BackendKind::TextureCreate:
		return "TextureCreate";
	case BackendKind::FramebufferCreate:
		return "FramebufferCreate";
	case BackendKind::TextureRelease:
		return "TextureRelease";
	case BackendKind::ViewRelease:
		return "ViewRelease";
	case BackendKind::FramebufferRelease:
		return "FramebufferRelease";
	case BackendKind::SurfaceCopy:
		return "SurfaceCopy";
	case BackendKind::BufferUpload:
		return "BufferUpload";
	case BackendKind::IndexCreate:
		return "IndexCreate";
	case BackendKind::IndexDeferredCreate:
		return "IndexDeferredCreate";
	case BackendKind::IndexUpload:
		return "IndexUpload";
	case BackendKind::IndexRelease:
		return "IndexRelease";
	case BackendKind::TexturePlane:
		return "TexturePlane";
	case BackendKind::BufferInit:
		return "BufferInit";
	case BackendKind::DebugSnapshot:
		return "DebugSnapshot";
	case BackendKind::NativeRelease:
		return "NativeRelease";
	default:
		return "Unknown";
	}
}

// These operations neither emit commands nor mutate inputs of captured draws.
inline bool PreservesDrawBatch(BackendKind kind)
{
	switch (kind)
	{
	case BackendKind::BindVertices:
	case BackendKind::BindFramebuffer:
	case BackendKind::Viewport:
	case BackendKind::Scissor:
	case BackendKind::IndexCreate:
	case BackendKind::IndexDeferredCreate:
	case BackendKind::IndexRelease:
	case BackendKind::RegistryRetire:
	case BackendKind::occlusionQuery_updateState:
		return true;
	default:
		return false;
	}
}
inline bool DrainsDrawBatch(BackendKind kind, bool readback = false, bool pendingPresent = false)
{
	if (kind == BackendKind::EndSequence)
		return readback;
	if (kind == BackendKind::ServicePresent)
		return pendingPresent;
	return !PreservesDrawBatch(kind);
}
} // namespace LattePipeline
