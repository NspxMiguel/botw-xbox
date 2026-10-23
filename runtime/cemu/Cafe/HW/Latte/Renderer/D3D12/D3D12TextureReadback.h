// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Ported from PrismFox67/Cemu-UWP; see runtime/cemu/VENDORED.md.

#pragma once
#include "Cafe/HW/Latte/Renderer/D3D12/LattePipeline.h"

#include "Cafe/HW/Latte/Core/LatteTextureReadbackInfo.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"

class D3D12Renderer;

class LatteTextureReadbackInfoD3D12 : public LatteTextureReadbackInfo
{
  public:
	LatteTextureReadbackInfoD3D12(D3D12Renderer *renderer, LatteTextureView *textureView);
	~LatteTextureReadbackInfoD3D12() override;

	uint32 GetImageSize() const
	{
		return m_image_size;
	}

	void StartTransfer() override;
	bool IsFinished() override;
	void ForceFinish() override;
	uint8 *GetData() override;

  private:
	friend class D3D12Renderer;
	LattePipeline::ResourceId m_recordingId;
	void Dispatch(uint32 action);
	void RecordStart(LattePipeline::ResourceId texture, uint32 mip, uint32 slice, uint32 rowPitch, uint32 imageSize, uint32 width, uint32 height);
	bool RecordFinished();
	void RecordForceFinish();
	bool m_replyFinished = false;
	uint8 *m_replyData = nullptr;

	D3D12Renderer *m_renderer;
	uint64 m_bufferOffset = 0;
	uint64 m_submissionId = 0;
};
