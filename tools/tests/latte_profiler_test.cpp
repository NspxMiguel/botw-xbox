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
	}
	assert(s_state.current == Slot::Pm4);
	assert(s_state.drawDepth == 0);
	assert(s_state.drawTotalNs == TotalExclusiveTime());

	s_state.enabled = false;
	const auto disabledTime = s_state.last;
	{
		Scope disabled(Slot::DrawCall);
		assert(s_state.current == Slot::Pm4);
	}
	assert(s_state.last == disabledTime);
}
