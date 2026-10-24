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
	if (s_state.detailEnabled)
		CheckpointDetails(now);
	constexpr size_t count = static_cast<size_t>(Slot::Count);
	constexpr std::array<const char *, count> names{
		"draw-other", "shader-pipeline", "texture-bind", "texture-load", "buffer-sync", "indices",
		"uniforms", "descriptors", "render-targets", "submit", "pm4",
		"draw-state", "pipeline-bind", "vertex-buffers", "root-signature", "render-state", "draw-call", "root-cbv", "barriers"};
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
	const auto backMs = [&](Slot slot) { return s_state.backTimeNs[static_cast<size_t>(slot)] * msPerNsPerFrame; };
	int64_t backCategorizedNs = 0;
	for (const auto slot : {Slot::Descriptors, Slot::RootCBV, Slot::VertexBuffers, Slot::RenderState, Slot::Barriers,
							Slot::DrawCall, Slot::Submit, Slot::Uniforms, Slot::RenderTargets, Slot::PipelineBind, Slot::RootSignature})
		backCategorizedNs += s_state.backTimeNs[static_cast<size_t>(slot)];
	const double backOtherMs = std::max<int64_t>(0, int64_t(s_state.backRecordWallNs) - backCategorizedNs) * msPerNsPerFrame;
	const auto perFrame = [&](Counter counter) { return (double)s_state.counters[static_cast<size_t>(counter)] / frames; };
	const double covered = std::clamp(coveredNs / (seconds * 1e9), 0.0, 1.0);
	cemuLog_log(LogType::Force,
				"Latte profile: {:.2f}s, {} game frames, {:.1f} draws/frame; ms/frame: draw-total {:.3f} (inclusive), "
				"draw-other {:.3f}, shader-pipeline {:.3f}, texture-bind {:.3f}, texture-load {:.3f}, buffer-sync {:.3f}, "
				"indices {:.3f}, uniforms {:.3f}, descriptors {:.3f}, render-targets {:.3f}, submit {:.3f}, pm4 {:.3f}, "
				"draw-state {:.3f}, pipeline-bind {:.3f}, vertex-buffers {:.3f}, root-signature {:.3f}, render-state {:.3f}, draw-call {:.3f}; "
				"top: {} {:.3f}, {} {:.3f}, {} {:.3f}; covered {:.1f}%, unaccounted {:.1f}% ({:.3f} ms/frame); "
				"pipeline-miss/frame {:.2f}, texture-new/frame {:.2f}, texture-upload {:.0f} B/frame, buffer-upload {:.0f} B/frame; "
				"descriptor-hit/frame {:.1f}, descriptor-fast/frame {:.1f}, descriptor-miss/frame {:.1f}; root-cbv-set/frame {:.1f}, root-cbv-skip/frame {:.1f}; "
				"dynamic-state-hit/frame {:.1f}, dynamic-state-miss/frame {:.1f}; "
				"transition-hit/frame {:.1f}, transition-miss/frame {:.1f}; index-hit/frame {:.1f}, index-miss/frame {:.1f}; fbo-hit/frame {:.1f}, fbo-miss/frame {:.1f}, pipeline-state-hit/frame {:.1f}; submit/frame {:.2f}, submit-empty/frame {:.2f}; "
				"index-content/frame {:.1f}, index-eviction/frame {:.1f}, index-transient/frame {:.1f}, index-relocated/frame {:.1f}, index-compared {:.0f} B/frame, index-hashed {:.0f} B/frame; "
				"texture-stage-hit/frame {:.1f}, texture-stage-miss/frame {:.1f}, texture-lookup-hit/frame {:.1f}, texture-lookup-miss/frame {:.1f}, texture-check/frame {:.1f}, texture-alias/frame {:.1f}, sequence/frame {:.1f}; "
				"register-packet/frame {:.1f}, register-word/frame {:.1f}, register-changed/frame {:.1f}, ring-word/frame {:.1f}, ring-refill/frame {:.1f}; "
				"submit-wait/frame {:.2f}, submit-limit/frame {:.2f}, submit-readback/frame {:.2f}, submit-overflow/frame {:.2f}; "
				"sequence-end/frame {:.1f}, create-reply/frame {:.1f}, forced-readback/frame {:.1f}, query-wait/frame {:.1f}, upload {:.0f} B/frame (index {:.0f}, uniform {:.0f}); operation {}, source-frame {}, recorded/submitted/completed {}/{}/{}; front-busy {:.3f}, back-busy {:.3f}, back-record-wall {:.3f}, front-backpressure {:.3f}, front-reply-wait {:.3f}, back-idle {:.3f} ms/frame, ring-high-water {}; front/back CPU {:.1f}/{:.1f}%; back ms/frame: descriptors {:.3f}, root-cbv {:.3f}, vertex-buffers {:.3f}, render-state {:.3f}, barriers {:.3f}, draw-call {:.3f}, submit {:.3f}, uniforms {:.3f}, render-targets {:.3f}, pipeline-bind {:.3f}, root-signature {:.3f}, other {:.3f}",
				seconds, frames, perFrame(Counter::Draws), s_state.drawTotalNs * msPerNsPerFrame,
				ms(Slot::DrawOther), ms(Slot::ShaderPipeline), ms(Slot::TextureBinding), ms(Slot::TextureLoad), ms(Slot::BufferSync),
				ms(Slot::Indices), ms(Slot::Uniforms), ms(Slot::Descriptors), ms(Slot::RenderTargets), ms(Slot::Submit), ms(Slot::Pm4),
				ms(Slot::DrawStateDecode), ms(Slot::PipelineBind), ms(Slot::VertexBuffers), ms(Slot::RootSignature), ms(Slot::RenderState), ms(Slot::DrawCall),
				names[order[0]], s_state.timeNs[order[0]] * msPerNsPerFrame,
				names[order[1]], s_state.timeNs[order[1]] * msPerNsPerFrame,
				names[order[2]], s_state.timeNs[order[2]] * msPerNsPerFrame,
				covered * 100.0, (1.0 - covered) * 100.0, (1.0 - covered) * seconds * 1000.0 / frames,
				perFrame(Counter::PipelineMisses), perFrame(Counter::TextureMisses), perFrame(Counter::TextureBytes), perFrame(Counter::BufferBytes),
				perFrame(Counter::DescriptorHits), perFrame(Counter::DescriptorFastHits), perFrame(Counter::DescriptorMisses), perFrame(Counter::RootCBVSets), perFrame(Counter::RootCBVSkips),
				perFrame(Counter::DynamicStateHits), perFrame(Counter::DynamicStateMisses),
				perFrame(Counter::TransitionHits), perFrame(Counter::TransitionMisses), perFrame(Counter::IndexHits), perFrame(Counter::IndexMisses),
				perFrame(Counter::FramebufferHits), perFrame(Counter::FramebufferMisses), perFrame(Counter::PipelineStateHits), perFrame(Counter::Submissions), perFrame(Counter::EmptySubmissions),
				perFrame(Counter::IndexContentMisses), perFrame(Counter::IndexSetEvictions), perFrame(Counter::IndexTransientMisses), perFrame(Counter::IndexRelocatedHits), perFrame(Counter::IndexComparedBytes), perFrame(Counter::IndexHashedBytes),
				perFrame(Counter::TextureStageHits), perFrame(Counter::TextureStageMisses), perFrame(Counter::TextureLookupHits), perFrame(Counter::TextureLookupMisses), perFrame(Counter::TextureContentChecks), perFrame(Counter::TextureAliasRefreshes), perFrame(Counter::DrawSequences),
				perFrame(Counter::RegisterPackets), perFrame(Counter::RegisterWords), perFrame(Counter::RegisterChangedPackets), perFrame(Counter::RingWords), perFrame(Counter::RingRefills),
				perFrame(Counter::SubmissionWaits), perFrame(Counter::SubmissionDrawLimit), perFrame(Counter::SubmissionReadback), perFrame(Counter::SubmissionOverflow),
				perFrame(Counter::SequenceEndings), perFrame(Counter::ResourceCreateReplies), perFrame(Counter::ForcedReadbacks), perFrame(Counter::QueryWaits), perFrame(Counter::UploadBytes), perFrame(Counter::IndexUploadBytes), perFrame(Counter::UniformUploadBytes), s_state.operationSequence, s_state.sourceFrame, s_state.recordedSequence, s_state.submittedSequence, s_state.completedSequence,
				s_state.frontBusyNs * msPerNsPerFrame, s_state.backBusyNs * msPerNsPerFrame, s_state.backRecordWallNs * msPerNsPerFrame,
				s_state.frontStallNs * msPerNsPerFrame, s_state.frontReplyNs * msPerNsPerFrame,
				s_state.backIdleNs * msPerNsPerFrame, s_state.ringHighWater,
				s_state.frontCpuNs / (seconds * 1e7), s_state.backCpuNs / (seconds * 1e7),
				backMs(Slot::Descriptors), backMs(Slot::RootCBV), backMs(Slot::VertexBuffers), backMs(Slot::RenderState),
				backMs(Slot::Barriers), backMs(Slot::DrawCall), backMs(Slot::Submit), backMs(Slot::Uniforms),
				backMs(Slot::RenderTargets), backMs(Slot::PipelineBind), backMs(Slot::RootSignature),
				backOtherMs);
	if (s_state.detailEnabled)
	{
		const auto detail = [&](Detail slot) { return s_state.detailNs[static_cast<size_t>(slot)] * msPerNsPerFrame; };
		cemuLog_log(LogType::Force, "Latte detail (inclusive ms/frame): sequence {:.3f}, shader-resolve {:.3f}, packet-decode {:.3f}, streamout {:.3f}, bindings-prepare {:.3f}",
					detail(Detail::Sequence), detail(Detail::ShaderResolve), detail(Detail::PacketDecode), detail(Detail::Streamout), detail(Detail::BindingsPrepare));
		cemuLog_log(LogType::Force, "Latte detail (inclusive ms/frame): sequence-state {:.3f}, sequence-finish {:.3f}, draw-setup {:.3f}, index-lookup-tail {:.3f}, index-hit {:.3f}, index-miss-lookup {:.3f}, index-convert {:.3f}, texture-stage {:.3f}, texture-resolve {:.3f}, texture-content {:.3f}, texture-aliases {:.3f}, pm4-registers {:.3f}; submit-close {:.3f}, submit-execute {:.3f}, submit-signal {:.3f}, submit-retire {:.3f}, submit-reset {:.3f}, submit-wait {:.3f}",
					detail(Detail::SequenceState), detail(Detail::SequenceFinish), detail(Detail::DrawSetup), detail(Detail::IndexLookup), detail(Detail::IndexHit), detail(Detail::IndexMiss), detail(Detail::IndexConvert), detail(Detail::TextureStage), detail(Detail::TextureResolve), detail(Detail::TextureContent), detail(Detail::TextureAliases), detail(Detail::Pm4Registers),
					detail(Detail::SubmitClose), detail(Detail::SubmitExecute), detail(Detail::SubmitSignal), detail(Detail::SubmitRetire), detail(Detail::SubmitReset), detail(Detail::SubmitWait));
	}
	cemuLog_log(LogType::Force, "Latte index misses/frame: content {:.1f}, set-eviction {:.1f}, transient {:.1f}",
				perFrame(Counter::IndexContentMisses), perFrame(Counter::IndexSetEvictions), perFrame(Counter::IndexTransientMisses));
	Reset(now);
}
