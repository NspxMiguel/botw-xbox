#include "Cafe/HW/Latte/Core/LatteProfiler.h"

#include <cassert>
#include <numeric>

using namespace LatteProfiler;

static int64_t TotalExclusiveTime()
{
	return std::accumulate(s_state.timeNs.begin(), s_state.timeNs.end(), int64_t{0});
}

int main()
{
	// Enable just this test thread without relying on process environment or game startup.
	s_state = {};
	s_state.enabled = true;
	s_state.current = Slot::Pm4;
	s_state.last = Clock::now();
	const auto start = s_state.last;
	{
		Scope draw(Slot::DrawOther);
		Scope decode(Slot::DrawStateDecode);
		const auto beforeSameSlot = s_state.last;
		{
			Scope sameDecode(Slot::DrawStateDecode);
			assert(s_state.last == beforeSameSlot);
			assert(s_state.drawDepth == 1);
		}
		assert(s_state.last == beforeSameSlot);
		for (const auto slot : {Slot::PipelineBind, Slot::VertexBuffers, Slot::RootSignature, Slot::RenderState, Slot::DrawCall, Slot::Descriptors})
		{
			{
				Scope child(slot);
				assert(s_state.current == slot);
				assert(s_state.drawDepth == 1);
			}
			assert(s_state.current == Slot::DrawStateDecode);
		}
	}
	assert(s_state.current == Slot::Pm4);
	assert(s_state.drawDepth == 0);
	assert(TotalExclusiveTime() == std::chrono::duration_cast<std::chrono::nanoseconds>(s_state.last - start).count());
	assert(s_state.drawTotalNs == TotalExclusiveTime() - s_state.timeNs[static_cast<size_t>(Slot::Pm4)]);

	// Combined draw entry preserves exclusive accounting and the inclusive union,
	// including nested draws and a reporting boundary inside a nested stage.
	Reset(Clock::now());
	const auto combinedStart = s_state.last;
	{
		DrawScope draw(Slot::DrawStateDecode);
		assert(s_state.drawDepth == 1);
		{
			Scope descriptors(Slot::Descriptors);
			DrawScope nested(Slot::DrawStateDecode);
			assert(s_state.drawDepth == 2);
		}
		assert(s_state.current == Slot::DrawStateDecode);
		assert(s_state.drawDepth == 1);
	}
	assert(s_state.current == Slot::Pm4 && s_state.drawDepth == 0);
	assert(TotalExclusiveTime() == std::chrono::duration_cast<std::chrono::nanoseconds>(s_state.last - combinedStart).count());
	assert(s_state.drawTotalNs == TotalExclusiveTime() - s_state.timeNs[size_t(Slot::Pm4)]);
	{
		DrawScope draw(Slot::DrawStateDecode);
		Checkpoint(Clock::now());
		Reset(Clock::now());
		assert(s_state.drawDepth == 1);
	}
	assert(s_state.drawTotalNs == TotalExclusiveTime());

	for (size_t i = 0; i < static_cast<size_t>(Counter::Count); ++i)
	{
		Add(static_cast<Counter>(i), i + 1);
		assert(s_state.counters[i] == i + 1);
	}

	// Reporting/resetting a window inside a new slot must preserve the active hierarchy.
	{
		Scope draw(Slot::DrawOther);
		Scope descriptors(Slot::Descriptors);
		const auto boundary = Clock::now();
		Checkpoint(boundary);
		Reset(boundary);
		assert(s_state.current == Slot::Descriptors);
		assert(s_state.drawDepth == 1);
		assert(TotalExclusiveTime() == 0);
		assert(s_state.drawTotalNs == 0);
		for (auto value : s_state.counters)
			assert(value == 0);
	}
	assert(s_state.current == Slot::Pm4);
	assert(s_state.drawDepth == 0);
	assert(s_state.drawTotalNs == TotalExclusiveTime());

	// Diagnostic sub-slots do not affect exclusive accounting. A reset inside
	// a live sub-slot rebases it, and same-detail nesting counts only once.
	s_state.detailEnabled = true;
	const auto exclusiveBeforeDetail = TotalExclusiveTime();
	{
		DetailScope outer(Detail::Sequence);
		{
			DetailScope inner(Detail::Sequence);
			assert(s_state.detailDepth[size_t(Detail::Sequence)] == 2);
		}
		const auto now = Clock::now();
		CheckpointDetails(now);
		assert(s_state.detailDepth[size_t(Detail::Sequence)] == 1);
		assert(s_state.detailStart[size_t(Detail::Sequence)] == now);
		Reset(now);
		assert(s_state.detailNs[size_t(Detail::Sequence)] == 0);
		assert(s_state.detailStart[size_t(Detail::Sequence)] == now);
	}
	assert(s_state.detailDepth[size_t(Detail::Sequence)] == 0);
	assert(s_state.detailNs[size_t(Detail::Sequence)] >= 0);
	assert(TotalExclusiveTime() == 0);
	(void)exclusiveBeforeDetail;

	// Outcome classification closes once, survives a window boundary and never
	// changes exclusive accounting. An explicit End is safe before destruction.
	for (size_t i = 0; i < size_t(Detail::Count); ++i)
	{
		DetailScope scope(static_cast<Detail>(i));
		CheckpointDetails(Clock::now());
		Reset(Clock::now());
		scope.End(Detail::IndexHit);
		scope.End(Detail::IndexHit);
		assert(s_state.detailDepth[i] == 0);
	}

	// Worker categories and counters survive transport independently of front coverage.
	State back{};
	for (size_t i = 0; i < back.timeNs.size(); ++i)
		back.timeNs[i] = 100 + i;
	back.counters[size_t(Counter::RootCBVSets)] = 19;
	const auto frontSlots = s_state.timeNs;
	AcceptBackProfile(back);
	assert(s_state.timeNs == frontSlots);
	assert(s_state.backTimeNs == back.timeNs);
	assert(s_state.counters[size_t(Counter::RootCBVSets)] == 19);
	Reset(Clock::now());
	for (auto ns : s_state.backTimeNs)
		assert(ns == 0);

	s_state.enabled = false;
	Add(Counter::DescriptorHits);
	assert(s_state.counters[static_cast<size_t>(Counter::DescriptorHits)] == 0);
	const auto disabledTime = s_state.last;
	{
		Scope disabled(Slot::DrawCall);
		DrawScope disabledDraw(Slot::DrawStateDecode);
		assert(s_state.current == Slot::Pm4);
		assert(s_state.drawDepth == 0);
	}
	assert(s_state.last == disabledTime);
}
