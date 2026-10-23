// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Ported from PrismFox67/Cemu-UWP; see runtime/cemu/VENDORED.md.

#pragma once

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"

// content of one per-stage CBV/SRV/UAV descriptor table, indexed by shader register
struct D3D12StageTableDesc
{
	D3D12_GPU_VIRTUAL_ADDRESS cbvAddress[D3D12Const::kMaxCBVsPerStage]{};
	uint32 cbvSize[D3D12Const::kMaxCBVsPerStage]{};
	D3D12_CPU_DESCRIPTOR_HANDLE srv[D3D12Const::kMaxSRVsPerStage]{};
	ID3D12Resource *uavResource = nullptr;
	uint32 uavSize = 0;
	bool operator==(const D3D12StageTableDesc &other) const
	{
		return std::memcmp(cbvAddress, other.cbvAddress, sizeof(cbvAddress)) == 0 &&
			   std::memcmp(cbvSize, other.cbvSize, sizeof(cbvSize)) == 0 &&
			   std::memcmp(srv, other.srv, sizeof(srv)) == 0 && uavResource == other.uavResource && uavSize == other.uavSize;
	}
};
