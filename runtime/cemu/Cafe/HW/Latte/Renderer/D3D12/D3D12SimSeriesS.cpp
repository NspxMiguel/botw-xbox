// SPDX-License-Identifier: MIT
#ifdef ENABLE_D3D12
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12SimSeriesS.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ShaderCompiler.h"
#include "../../../../../../src/sim_series_s.h"
#include "../../../../../../src/log.h"
#include <cstring>

namespace
{
// Dependent integer arithmetic and distinct UAV stores keep the ALU work observable.
// Current-frame timestamps are consumed on the GPU, without a host readback round trip.
constexpr const char *shader = R"hlsl(
ByteAddressBuffer stamps : register(t0);
RWByteAddressBuffer result : register(u0);
cbuffer Params : register(b0) {
    uint segments;
    float extra;
    float loops_per_tick;
    float tick_limit;
};
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float elapsed = 0;
    for (uint s = 0; s < segments; ++s) {
        uint2 a = stamps.Load2(s * 16);
        uint2 b = stamps.Load2(s * 16 + 8);
        if (b.y < a.y || (b.y == a.y && b.x < a.x))
            continue; // timestamp discontinuity: skip rather than wrap to a huge value
        uint lo = b.x - a.x;
        uint hi = b.y - a.y - uint(b.x < a.x);
        elapsed += float(lo) + float(hi) * 4294967296.0;
    }
    float target = min(elapsed * extra, tick_limit);
    float requested = target * loops_per_tick;
    uint count = target <= 0 ? 0 : (loops_per_tick > 0 ? uint(clamp(requested, 1.0, 4194304.0)) : 4096);
    uint value = id.x + 17;
    [loop] for (uint n = 0; n < count; ++n) {
        value ^= value >> 13;
        value = value * 1664525u + 1013904223u;
        value ^= value << 7;
    }
    result.Store(8 + id.x * 4, value);
    if (id.x == 0) result.Store2(0, uint2(count, uint(elapsed * extra >= tick_limit || requested >= 4194304.0)));
}
)hlsl";
ComPtr<ID3D12Resource> buffer(ID3D12Device *device, uint64_t size, D3D12_HEAP_TYPE type,
							  D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
	D3D12_HEAP_PROPERTIES properties{};
	properties.Type = type;
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = size;
	desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = flags;
	ComPtr<ID3D12Resource> resource;
	D3D12_ThrowIfFailed(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
														IID_PPV_ARGS(&resource)),
						"simulation buffer");
	return resource;
}
} // namespace
D3D12SimSeriesS::D3D12SimSeriesS(D3D12Renderer &renderer_, ID3D12CommandQueue *queue) : renderer(renderer_)
{
	auto *device = renderer.GetDevice();
	D3D12_ThrowIfFailed(queue->GetTimestampFrequency(&frequency), "simulation timestamp frequency");
	if (!frequency)
		throw std::runtime_error("Simulation timestamp frequency is zero");
	D3D12_QUERY_HEAP_DESC desc{};
	desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	desc.Count = kSlots * kQueries;
	D3D12_ThrowIfFailed(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap)), "simulation queries");
	if (sim_series_s::settings().enabled && sim_series_s::settings().gpu_factor > 1)
	{
		D3D12_ROOT_PARAMETER params[3]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants = {0, 0, 4};
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
		D3D12_ROOT_SIGNATURE_DESC rootDesc{};
		rootDesc.NumParameters = 3;
		rootDesc.pParameters = params;
		ComPtr<ID3DBlob> blob, errors;
		D3D12_ThrowIfFailed(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "simulation root serialization");
		D3D12_ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)), "simulation root");
		std::vector<uint8> dxil;
		std::string log;
		if (!D3D12ShaderCompiler::CompileComputeHLSL(shader, dxil, log))
			throw std::runtime_error("Simulation DXC: " + log);
		D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
		pso.pRootSignature = root.Get();
		pso.CS = {dxil.data(), dxil.size()};
		D3D12_ThrowIfFailed(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline)), "simulation pipeline");
	}
	for (auto &slot : slots)
	{
		slot.timestamps = buffer(device, kBallastOffset, D3D12_HEAP_TYPE_DEFAULT, slot.timestampState);
		slot.output = buffer(device, 8 + 128 * 64 * 4, D3D12_HEAP_TYPE_DEFAULT, slot.outputState, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		slot.readback = buffer(device, kBallastOffset + 24, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
		const D3D12_RANGE range{0, kBallastOffset + 24};
		D3D12_ThrowIfFailed(slot.readback->Map(0, &range, reinterpret_cast<void **>(&slot.mapped)), "simulation readback map");
	}
}
D3D12SimSeriesS::~D3D12SimSeriesS()
{
	// Renderer drains the direct queue before destroying us.
	const D3D12_RANGE written{0, 0};
	for (auto &slot : slots)
		if (slot.mapped)
			slot.readback->Unmap(0, &written);
}
void D3D12SimSeriesS::Poll()
{
	const auto completed = renderer.GetCompletedSubmissionId();
	for (auto &slot : slots)
	{
		if (!slot.pending || slot.fence > completed)
			continue;
		uint64_t ticks = 0;
		for (uint32_t i = 0; i < slot.segments; ++i)
		{
			uint64_t pair[2];
			std::memcpy(pair, slot.mapped + i * 16, 16);
			if (pair[1] >= pair[0])
				ticks += pair[1] - pair[0];
		}
		const double frameMs = double(ticks) * 1000 / frequency;
		gpuMs += frameMs;
		maxGpuMs = std::max(maxGpuMs, frameMs);
		if (slot.ballast)
		{
			uint64_t pair[2];
			uint32_t info[2];
			std::memcpy(pair, slot.mapped + kBallastOffset, 16);
			std::memcpy(info, slot.mapped + kBallastOffset + 16, 8);
			if (pair[1] > pair[0])
			{
				const double duration = double(pair[1] - pair[0]);
				rate = sim_series_s::smooth_rate(rate, info[0], duration);
				ballastMs += duration * 1000 / frequency;
			}
			capped += info[1] != 0;
		}
		++samples;
		slot.pending = false;
	}
}
void D3D12SimSeriesS::BeginSegment()
{
	if (skippedFrame)
		return;
	if (current < 0)
	{
		Poll();
		const auto completed = renderer.GetCompletedSubmissionId();
		for (uint32_t i = 0; i < kSlots; ++i)
		{
			if (!slots[i].pending && slots[i].fence <= completed)
			{
				current = int(i);
				slots[i].segments = 0;
				break;
			}
		}
		if (current < 0)
		{
			skippedFrame = true;
			return;
		}
	}
	auto &slot = slots[current];
	if (slot.segments == kSegments)
		return;
	renderer.GetCommandList()->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, current * kQueries + slot.segments * 2);
	segmentOpen = true;
}
void D3D12SimSeriesS::EndSegment()
{
	if (!segmentOpen)
		return;
	auto &slot = slots[current];
	renderer.GetCommandList()->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, current * kQueries + slot.segments * 2 + 1);
	++slot.segments;
	slot.fence = renderer.GetRecordingSubmissionId();
	segmentOpen = false;
}
void D3D12SimSeriesS::EndFrame()
{
	renderer.FlushBarriers();
	EndSegment();
	Poll();
	++frames;
	if (current >= 0)
	{
		auto &slot = slots[current];
		auto *cmd = renderer.GetCommandList();
		const uint32_t first = current * kQueries;
		cmd->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, slot.segments * 2, slot.readback.Get(), 0);
		slot.ballast = sim_series_s::settings().enabled && sim_series_s::settings().gpu_factor > 1;
		if (slot.ballast)
		{
			renderer.TransitionResource(slot.timestamps.Get(), slot.timestampState, D3D12_RESOURCE_STATE_COPY_DEST);
			renderer.FlushBarriers();
			cmd->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, slot.segments * 2, slot.timestamps.Get(), 0);
			renderer.TransitionResource(slot.timestamps.Get(), slot.timestampState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			renderer.TransitionResource(slot.output.Get(), slot.outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			renderer.FlushBarriers();
			cmd->SetPipelineState(pipeline.Get());
			cmd->SetComputeRootSignature(root.Get());
			struct Constants
			{
				uint32_t segments;
				float extra, rate, limit;
			} constants{
				slot.segments, float(sim_series_s::settings().gpu_factor - 1), float(rate), float(frequency * sim_series_s::kMaxExtraSeconds)};
			cmd->SetComputeRoot32BitConstants(0, 4, &constants, 0);
			cmd->SetComputeRootShaderResourceView(1, slot.timestamps->GetGPUVirtualAddress());
			cmd->SetComputeRootUnorderedAccessView(2, slot.output->GetGPUVirtualAddress());
			cmd->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + kSegments * 2);
			cmd->Dispatch(128, 1, 1);
			cmd->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + kSegments * 2 + 1);
			cmd->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + kSegments * 2, 2, slot.readback.Get(), kBallastOffset);
			renderer.TransitionResource(slot.output.Get(), slot.outputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
			renderer.FlushBarriers();
			cmd->CopyBufferRegion(slot.readback.Get(), kBallastOffset + 16, slot.output.Get(), 0, 8);
			renderer.InvalidateDrawState();
		}
		if (slot.segments == kSegments)
		{
			LOG("GPU timestamp segment limit reached; frame is underweighted");
			++capped;
		}
		slot.fence = renderer.GetRecordingSubmissionId();
		slot.pending = true;
		current = -1;
	}
	else
		++skipped;
	skippedFrame = false;
	const auto now = std::chrono::steady_clock::now();
	const double seconds = std::chrono::duration<double>(now - lastLog).count();
	if (seconds >= 1)
	{
		LOG("GPU timing: mean %.3f ms, max %.3f ms, samples %llu, capped %llu, skipped %llu",
			samples ? gpuMs / samples : 0, maxGpuMs,
			static_cast<unsigned long long>(samples), static_cast<unsigned long long>(capped), static_cast<unsigned long long>(skipped));
		if (sim_series_s::settings().enabled)
		{
			const auto cpu = sim_series_s::take_cpu_stats();
			LOG("Series S simulation: fps %.1f, cpu %.3f ms, gpu %.3f -> %.3f ms (samples %llu, capped %llu, skipped %llu)",
				frames / seconds, cpu.frames ? cpu.work_ms / cpu.frames : 0,
				samples ? gpuMs / samples : 0, samples ? (gpuMs + ballastMs) / samples : 0,
				static_cast<unsigned long long>(samples), static_cast<unsigned long long>(capped), static_cast<unsigned long long>(skipped));
		}
		lastLog = now;
		gpuMs = ballastMs = maxGpuMs = 0;
		samples = frames = capped = skipped = 0;
	}
}
#endif
