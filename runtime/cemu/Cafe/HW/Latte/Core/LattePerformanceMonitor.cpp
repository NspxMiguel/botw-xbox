#include "Cafe/HW/Latte/Core/LatteProfiler.h"

#include <algorithm>

#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "WindowSystem.h"

performanceMonitor_t performanceMonitor{};

void LattePerformanceMonitor_frameEnd()
{
	// per-frame stats
	performanceMonitor.gpuTime_shaderCreate.frameFinished();
	performanceMonitor.gpuTime_frameTime.frameFinished();
	performanceMonitor.gpuTime_idleTime.frameFinished();
	performanceMonitor.gpuTime_fenceTime.frameFinished();

	performanceMonitor.gpuTime_dcStageTextures.frameFinished();
	performanceMonitor.gpuTime_dcStageVertexMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageShaderAndUniformMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageIndexMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageMRT.frameFinished();
	performanceMonitor.gpuTime_dcStageDrawcallAPI.frameFinished();
	performanceMonitor.gpuTime_waitForAsync.frameFinished();

	uint32 elapsedTime = GetTickCount() - performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate;
	if (elapsedTime >= 1000)
	{
		bool isFirstUpdate = performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate == 0;
		// sum up raw stats
		uint32 totalElapsedTime = GetTickCount() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastUpdate;
		uint32 totalElapsedTimeFPS = GetTickCount() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastUpdate;
		uint32 elapsedFrames = 0;
		uint32 elapsedFrames2S = 0; // elapsed frames for last two entries (seconds)
		uint64 skippedCycles = 0;
		uint64 vertexDataUploaded = 0;
		uint64 vertexDataCached = 0;
		uint64 uniformBankUploadedData = 0;
		uint64 uniformBankUploadedCount = 0;
		uint64 indexDataUploaded = 0;
		uint64 indexDataCached = 0;
		uint32 frameCounter = 0;
		uint32 drawCallCounter = 0;
		uint32 fastDrawCallCounter = 0;
		uint32 shaderBindCounter = 0;
		uint32 recompilerLeaveCount = 0;
		uint32 threadLeaveCount = 0;
		for (sint32 i = 0; i < PERFORMANCE_MONITOR_TRACK_CYCLES; i++)
		{
			elapsedFrames += performanceMonitor.cycle[i].frameCounter;
			skippedCycles += performanceMonitor.cycle[i].skippedCycles;
			vertexDataUploaded += performanceMonitor.cycle[i].vertexDataUploaded;
			vertexDataCached += performanceMonitor.cycle[i].vertexDataCached;
			uniformBankUploadedData += performanceMonitor.cycle[i].uniformBankUploadedData;
			uniformBankUploadedCount += performanceMonitor.cycle[i].uniformBankUploadedCount;
			indexDataUploaded += performanceMonitor.cycle[i].indexDataUploaded;
			indexDataCached += performanceMonitor.cycle[i].indexDataCached;
			frameCounter += performanceMonitor.cycle[i].frameCounter;
			drawCallCounter += performanceMonitor.cycle[i].drawCallCounter;
			fastDrawCallCounter += performanceMonitor.cycle[i].fastDrawCallCounter;
			shaderBindCounter += performanceMonitor.cycle[i].shaderBindCount;
			recompilerLeaveCount += performanceMonitor.cycle[i].recompilerLeaveCount;
			threadLeaveCount += performanceMonitor.cycle[i].threadLeaveCount;
		}
		elapsedFrames = std::max<uint32>(elapsedFrames, 1);
		elapsedFrames2S = performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 0) % PERFORMANCE_MONITOR_TRACK_CYCLES].frameCounter;
		elapsedFrames2S += performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].frameCounter;
		elapsedFrames2S = std::max<uint32>(elapsedFrames2S, 1);
		// calculate stats
		uint64 passedCycles = PPCInterpreter_getMainCoreCycleCounter() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastCycleCount;
		passedCycles -= skippedCycles;
		uint64 vertexDataUploadPerFrame = (vertexDataUploaded / (uint64)elapsedFrames);
		vertexDataUploadPerFrame /= 1024ULL;
		uint64 vertexDataCachedPerFrame = (vertexDataCached / (uint64)elapsedFrames);
		vertexDataCachedPerFrame /= 1024ULL;
		uint64 uniformBankDataUploadedPerFrame = (uniformBankUploadedData / (uint64)elapsedFrames);
		uniformBankDataUploadedPerFrame /= 1024ULL;
		uint32 uniformBankCountUploadedPerFrame = (uint32)(uniformBankUploadedCount / (uint64)elapsedFrames);
		uint64 indexDataUploadPerFrame = (indexDataUploaded / (uint64)elapsedFrames);

		double fps = (double)elapsedFrames2S * 1000.0 / (double)totalElapsedTimeFPS;
		uint32 shaderBindsPerFrame = shaderBindCounter / elapsedFrames;
		passedCycles = passedCycles * 1000ULL / totalElapsedTime;
		uint32 rlps = (uint32)((uint64)recompilerLeaveCount * 1000ULL / (uint64)totalElapsedTime);
		uint32 tlps = (uint32)((uint64)threadLeaveCount * 1000ULL / (uint64)totalElapsedTime);
		// set stats
		performanceMonitor.stats.indexDataUploadPerFrame = indexDataUploadPerFrame;
		// next counter cycle
		sint32 nextCycleIndex = (performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES;
		performanceMonitor.cycle[nextCycleIndex].drawCallCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].fastDrawCallCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].frameCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].shaderBindCount = 0;
		performanceMonitor.cycle[nextCycleIndex].lastCycleCount = PPCInterpreter_getMainCoreCycleCounter();
		performanceMonitor.cycle[nextCycleIndex].skippedCycles = 0;
		performanceMonitor.cycle[nextCycleIndex].vertexDataUploaded = 0;
		performanceMonitor.cycle[nextCycleIndex].vertexDataCached = 0;
		performanceMonitor.cycle[nextCycleIndex].uniformBankUploadedData = 0;
		performanceMonitor.cycle[nextCycleIndex].uniformBankUploadedCount = 0;
		performanceMonitor.cycle[nextCycleIndex].indexDataUploaded = 0;
		performanceMonitor.cycle[nextCycleIndex].indexDataCached = 0;
		performanceMonitor.cycle[nextCycleIndex].recompilerLeaveCount = 0;
		performanceMonitor.cycle[nextCycleIndex].threadLeaveCount = 0;
		performanceMonitor.cycleIndex = nextCycleIndex;

		// next update in 1 second
		performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate = GetTickCount();

		if (isFirstUpdate)
		{
			LatteOverlay_updateStats(0.0, 0, 0);
			WindowSystem::UpdateWindowTitles(false, false, 0.0);
		}
		else
		{
			LatteOverlay_updateStats(fps, drawCallCounter / elapsedFrames, fastDrawCallCounter / elapsedFrames);
			WindowSystem::UpdateWindowTitles(false, false, fps);
		}
	}
}

void LattePerformanceMonitor_frameBegin()
{
	performanceMonitor.vk.numDrawBarriersPerFrame.reset();
	performanceMonitor.vk.numBeginRenderpassPerFrame.reset();
}

void LatteProfiler::Report(Clock::time_point now, double seconds, uint64_t frames)
{
	if (!s_state.enabled || !frames || seconds <= 0.0)
		return;
	Checkpoint(now);
	constexpr size_t count = static_cast<size_t>(Slot::Count);
	constexpr std::array<const char *, count> names{
		"draw-other", "shader-pipeline", "texture-bind", "texture-load", "buffer-sync", "indices",
		"uniforms", "descriptors", "render-targets", "submit", "pm4"};
	std::array<size_t, count> order{};
	int64_t coveredNs = 0;
	for (size_t i = 0; i < count; ++i)
	{
		order[i] = i;
		coveredNs += s_state.timeNs[i];
	}
	std::sort(order.begin(), order.end(), [](size_t a, size_t b) {
		return s_state.timeNs[a] != s_state.timeNs[b] ? s_state.timeNs[a] > s_state.timeNs[b] : a < b;
	});
	const double msPerNsPerFrame = 1.0 / (1e6 * frames);
	const auto ms = [&](Slot slot) { return s_state.timeNs[static_cast<size_t>(slot)] * msPerNsPerFrame; };
	const auto perFrame = [&](Counter counter) { return (double)s_state.counters[static_cast<size_t>(counter)] / frames; };
	const double covered = std::clamp(coveredNs / (seconds * 1e9), 0.0, 1.0);
	cemuLog_log(LogType::Force,
				"Latte profile: {:.2f}s, {} game frames, {:.1f} draws/frame; ms/frame: draw-total {:.3f} (inclusive), "
				"draw-other {:.3f}, shader-pipeline {:.3f}, texture-bind {:.3f}, texture-load {:.3f}, buffer-sync {:.3f}, "
				"indices {:.3f}, uniforms {:.3f}, descriptors {:.3f}, render-targets {:.3f}, submit {:.3f}, pm4 {:.3f}; "
				"top: {} {:.3f}, {} {:.3f}, {} {:.3f}; covered {:.1f}%, unaccounted {:.1f}% ({:.3f} ms/frame); "
				"pipeline-miss/frame {:.2f}, texture-new/frame {:.2f}, texture-upload {:.0f} B/frame, buffer-upload {:.0f} B/frame",
				seconds, frames, perFrame(Counter::Draws), s_state.drawTotalNs * msPerNsPerFrame,
				ms(Slot::DrawOther), ms(Slot::ShaderPipeline), ms(Slot::TextureBinding), ms(Slot::TextureLoad), ms(Slot::BufferSync),
				ms(Slot::Indices), ms(Slot::Uniforms), ms(Slot::Descriptors), ms(Slot::RenderTargets), ms(Slot::Submit), ms(Slot::Pm4),
				names[order[0]], s_state.timeNs[order[0]] * msPerNsPerFrame,
				names[order[1]], s_state.timeNs[order[1]] * msPerNsPerFrame,
				names[order[2]], s_state.timeNs[order[2]] * msPerNsPerFrame,
				covered * 100.0, (1.0 - covered) * 100.0, (1.0 - covered) * seconds * 1000.0 / frames,
				perFrame(Counter::PipelineMisses), perFrame(Counter::TextureMisses), perFrame(Counter::TextureBytes), perFrame(Counter::BufferBytes));
	Reset(now);
}
