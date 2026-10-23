// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Ported from PrismFox67/Cemu-UWP; see runtime/cemu/VENDORED.md.

// BotWRecomp: iOS also globs this directory; keep non-D3D12 builds platform-neutral.
#ifdef ENABLE_D3D12

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12TextureReadback.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h"

LatteTextureReadbackInfoD3D12::LatteTextureReadbackInfoD3D12(D3D12Renderer *renderer, LatteTextureView *textureView)
	: LatteTextureReadbackInfo(textureView, textureView->firstMip), m_renderer(renderer)
{
	m_recordingId = m_renderer->RegisterResource(this);
	m_renderer->m_frontReadbacks[D3D12Renderer::ResourceKey(m_recordingId)] = this;
	auto *baseTexture = static_cast<LatteTextureD3D12 *>(textureView->baseTexture);
	if (!baseTexture->GetResource() || baseTexture->IsAlternateFormat() || baseTexture->IsCompressedFormat())
	{
		// same restriction as the Vulkan backend: the core expects the host data to use the guest format's layout
		cemuLog_logDebug(LogType::Force, "D3D12 does not support readback of texture format 0x{:x}", (uint32)baseTexture->format);
		m_image_size = 0;
		return;
	}
	m_rowPitch = GetReadbackRowPitch(textureView, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
	m_image_size = GetReadbackImageSize(textureView, m_rowPitch);
}

void LatteTextureReadbackInfoD3D12::StartTransfer()
{
	Dispatch(0);
	m_textureView = nullptr;
}

void LatteTextureReadbackInfoD3D12::RecordStart(LattePipeline::ResourceId texture, uint32 mip, uint32 slice, uint32 rowPitch, uint32 imageSize, uint32 width, uint32 height)
{
	auto *baseTexture = m_renderer->m_resources.Get<LatteTextureD3D12>(texture);

	m_bufferOffset = m_renderer->m_readbackRing->Allocate(imageSize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
	m_renderer->TransitionTexture(baseTexture, mip, 1, slice, 1, D3D12_RESOURCE_STATE_COPY_SOURCE);
	m_renderer->FlushBarriers();

	const uint32 sub = baseTexture->GetSubresourceIndex(mip, slice, 0);
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
	UINT numRows;
	UINT64 rowSize, totalBytes;
	m_renderer->GetDevice()->GetCopyableFootprints(&baseTexture->GetDesc(), sub, 1, 0, &layout, &numRows, &rowSize, &totalBytes);


	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = baseTexture->GetResource();
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src.SubresourceIndex = sub;
	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = m_renderer->m_readbackRing->GetResource();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint.Offset = m_bufferOffset;
	dst.PlacedFootprint.Footprint.Format = layout.Footprint.Format;
	dst.PlacedFootprint.Footprint.Width = std::min<uint32>(width, layout.Footprint.Width);
	dst.PlacedFootprint.Footprint.Height = std::min<uint32>(height, layout.Footprint.Height);
	dst.PlacedFootprint.Footprint.Depth = 1;
	dst.PlacedFootprint.Footprint.RowPitch = rowPitch;
	D3D12_BOX box{0, 0, 0, dst.PlacedFootprint.Footprint.Width, dst.PlacedFootprint.Footprint.Height, 1};
	m_renderer->GetCommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

	m_submissionId = m_renderer->GetRecordingSubmissionId();
	m_renderer->m_hasRecordedWork = true;
	// reduce readback latency by submitting soon
	m_renderer->RequestSubmitSoon();
}

bool LatteTextureReadbackInfoD3D12::RecordFinished()
{
	if (m_submissionId > m_renderer->m_lastSubmittedFenceValue)
		return false; // still in the command list that is being recorded
	return m_renderer->GetCompletedSubmissionId() >= m_submissionId;
}

void LatteTextureReadbackInfoD3D12::RecordForceFinish()
{
	if (m_submissionId > m_renderer->m_lastSubmittedFenceValue)
		m_renderer->SubmitCommandList(false);
	m_renderer->WaitForSubmission(m_submissionId);
}

uint8 *LatteTextureReadbackInfoD3D12::GetData()
{
	if (m_replyData.empty() && m_image_size)
		Dispatch(3);
	return m_replyData.data();
}

LatteTextureReadbackInfoD3D12::~LatteTextureReadbackInfoD3D12()
{
	m_renderer->RetireResource(m_recordingId);
	m_renderer->m_frontReadbacks.erase(D3D12Renderer::ResourceKey(m_recordingId));
}

bool LatteTextureReadbackInfoD3D12::IsFinished()
{
	m_renderer->m_operations.Poll();
	if (!m_pollPending && !m_replyFinished)
	{
		m_pollPending = true;
		Dispatch(1);
	}
	return m_replyFinished;
}
void LatteTextureReadbackInfoD3D12::ForceFinish()
{
	LatteProfiler::Add(LatteProfiler::Counter::ForcedReadbacks);
	Dispatch(2);
}
void LatteTextureReadbackInfoD3D12::Dispatch(uint32 action)
{
	m_renderer->m_pipeline.CheckFront();
	D3D12Renderer::ReadbackPacket packet{};
	packet.readback = m_recordingId;
	packet.action = action;
	if (action == 0)
	{
		cemu_assert(m_textureView);
		auto *texture = static_cast<LatteTextureD3D12 *>(m_textureView->baseTexture);
		cemu_assert_debug(!texture->Is3DTexture());
		packet.texture = texture->recordingId;
		packet.mip = m_firstMip;
		packet.slice = m_firstSlice;
		packet.rowPitch = m_rowPitch;
		packet.imageSize = m_image_size;
		packet.width = texture->GetMipWidth(m_firstMip);
		packet.height = texture->GetMipHeight(m_firstMip);
	}
	const auto sequence = m_renderer->m_operations.Execute<D3D12Renderer::ReadbackPacket, &D3D12Renderer::readback_record>(m_renderer->m_pipeline, LattePipeline::Opcode::Readback, packet, *m_renderer);
	if (action >= 2)
		m_renderer->m_operations.Wait(sequence);
}

void D3D12Renderer::AcceptReadbackResult(const LattePipeline::Completion &completion)
{
	const auto it = m_frontReadbacks.find(ResourceKey(completion.resource));
	if (it == m_frontReadbacks.end())
		return;
	auto *readback = it->second;
	readback->m_pollPending = false;
	readback->m_replyFinished = completion.words[0] != 0;
	m_frontReadbacksReady |= readback->m_replyFinished;
	if (!completion.bytes.empty())
	{
		readback->m_replyData.resize(completion.bytes.size());
		memcpy(readback->m_replyData.data(), completion.bytes.data(), completion.bytes.size());
	}
}

void D3D12Renderer::readback_record(const ReadbackPacket &packet, const LattePipeline::Arena &)
{
	m_pipeline.CheckBack();
	auto *readback = m_resources.Get<LatteTextureReadbackInfoD3D12>(packet.readback);
	switch (packet.action)
	{
	case 0:
		readback->RecordStart(packet.texture, packet.mip, packet.slice, packet.rowPitch, packet.imageSize, packet.width, packet.height);
		break;
	case 1:
		m_operations.Result().kind = ReadbackResult;
		m_operations.Result().resource = packet.readback;
		m_operations.Result().words[0] = readback->RecordFinished();
		break;
	case 2:
		readback->RecordForceFinish();
		m_operations.Result().kind = ReadbackResult;
		m_operations.Result().resource = packet.readback;
		m_operations.Result().words[0] = true;
		break;
	case 3:
		// Transfer owned bytes after GPU completion. Front never borrows mapped
		// native ring memory while the worker can allocate/overwrite it.
		readback->RecordForceFinish();
		m_operations.Result().kind = ReadbackResult;
		m_operations.Result().resource = packet.readback;
		m_operations.Result().words[0] = true;
		m_operations.Result().bytes.resize(readback->GetImageSize());
		memcpy(m_operations.Result().bytes.data(), m_readbackRing->GetCPUPtr(readback->m_bufferOffset), readback->GetImageSize());
		break;
	default:
		throw std::logic_error("Invalid Latte readback operation");
	}
	if (m_operations.Result().kind == ReadbackResult && m_operations.Result().words[0] && m_operations.Result().bytes.empty())
	{
		auto &bytes = m_operations.Result().bytes;
		bytes.resize(readback->GetImageSize());
		memcpy(bytes.data(), m_readbackRing->GetCPUPtr(readback->m_bufferOffset), bytes.size());
	}
	for (auto id : {packet.readback, packet.texture})
		m_resources.Use(id, m_operations.ActiveHeader().sequence, GetRecordingSubmissionId());
}

#endif // ENABLE_D3D12
