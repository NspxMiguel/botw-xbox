// BotWRecomp: host-facing D3D12 settings, independent of Cemu's frontend.
#pragma once
#include "config/CemuConfig.h"
#include <atomic>

struct D3D12RuntimeSettings
{
	uint64_t adapterLuid = 0;
	int vsync = 1;
	// 0 smooth: a draw whose pipeline is still compiling pops in a few frames later instead of
	// stalling the frame (balanced waited up to 2 s per new pipeline on the Xbox driver).
	int pipelineWait = 0;
	int shaderCompiler = 1; // DXC/DXIL by default; 0 selects upstream FXC.
	bool frameInterpolation = false;
};
inline D3D12RuntimeSettings &GetD3D12Settings()
{
	static D3D12RuntimeSettings settings;
	return settings;
}
// No display-mode controller is included in the recomp host.
inline std::atomic<double> g_hostDisplayModeRefreshHz{0.0};

// BotWRecomp: the full Cemu frontend normally supplies this pressure signal.
// Preserve throttling after allocation failure; the host can clear it after recovery.
inline std::atomic<int> g_d3d12MemoryPressure{0};
inline int D3D12GetMemoryPressure()
{
	return g_d3d12MemoryPressure.load();
}
inline void D3D12SetMemoryPressure(int pressure)
{
	g_d3d12MemoryPressure.store(pressure);
}
