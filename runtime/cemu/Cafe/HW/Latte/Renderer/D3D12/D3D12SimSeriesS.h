// SPDX-License-Identifier: MIT
#pragma once
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
#include <array>
#include <chrono>
class D3D12Renderer;

// Same direct queue as the game. Completed samples are polled; resource pressure skips
// instrumentation instead of introducing another CPU fence wait.
class D3D12SimSeriesS
{
  public:
	D3D12SimSeriesS(D3D12Renderer &renderer, ID3D12CommandQueue *queue);
	~D3D12SimSeriesS();
	void BeginSegment();
	void EndSegment();
	void EndFrame();

  private:
	static constexpr uint32_t kSlots = 8;
	static constexpr uint32_t kSegments = 1024;
	static constexpr uint32_t kQueries = kSegments * 2 + 2;
	static constexpr uint64_t kBallastOffset = kSegments * 16;
	struct Slot
	{
		ComPtr<ID3D12Resource> timestamps, output, readback;
		D3D12_RESOURCE_STATES timestampState = D3D12_RESOURCE_STATE_COPY_DEST;
		D3D12_RESOURCE_STATES outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		uint8_t *mapped = nullptr;
		uint64_t fence = 0;
		uint32_t segments = 0;
		bool pending = false;
		bool ballast = false;
	};
	void Poll();
	D3D12Renderer &renderer;
	ComPtr<ID3D12QueryHeap> heap;
	ComPtr<ID3D12RootSignature> root;
	ComPtr<ID3D12PipelineState> pipeline;
	std::array<Slot, kSlots> slots;
	int current = -1;
	bool segmentOpen = false;
	bool skippedFrame = false;
	uint64_t frequency = 0;
	double rate = 0, gpuMs = 0, ballastMs = 0;
	uint64_t samples = 0, frames = 0, capped = 0, skipped = 0;
	std::chrono::steady_clock::time_point lastLog = std::chrono::steady_clock::now();
};
