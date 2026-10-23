// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Ported from PrismFox67/Cemu-UWP; see runtime/cemu/VENDORED.md.

// BotWRecomp: iOS also globs this directory; keep non-D3D12 builds platform-neutral.
#ifdef ENABLE_D3D12

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Core/LatteQueryObject.h"

// D3D12 queries cannot span command lists. Like the Vulkan backend, a Latte query is split into fragments, one per
// command list, and the results are summed up once all fragments have completed.
class LatteQueryObjectD3D12 final : public LatteQueryObject
{
	friend class D3D12Renderer;

  public:
	explicit LatteQueryObjectD3D12(D3D12Renderer *renderer) : m_renderer(renderer)
	{
	}

	LattePipeline::ResourceId recordingId;
	bool getResult(uint64 &numSamplesPassed) override
	{
		Dispatch(0);
		if (m_resultReady)
			numSamplesPassed = m_result;
		return m_resultReady;
	}
	void begin() override
	{
		Dispatch(1);
	}
	void end() override
	{
		Dispatch(2);
	}
	void Dispatch(uint32 action)
	{
		m_renderer->m_pipeline.CheckFront();
		D3D12Renderer::QueryPacket packet{recordingId, action};
		m_renderer->m_operations.Execute<D3D12Renderer::QueryPacket, &D3D12Renderer::query_record>(m_renderer->m_pipeline, LattePipeline::Opcode::Query, packet, *m_renderer);
		m_renderer->ApplyFrontCompletions();
	}
	bool RecordResult(uint64 &numSamplesPassed)
	{
		if (!m_ended)
			return false;
		if (m_finishSubmission > m_renderer->m_lastSubmittedFenceValue || m_renderer->GetCompletedSubmissionId() < m_finishSubmission)
			return false;
		HandleFinishedFragments();
		cemu_assert_debug(m_fragments.empty());
		numSamplesPassed = m_accumulatedSum;
		return true;
	}

	void RecordBegin()
	{
		m_ended = false;
		m_hasActiveQuery = true;
		BeginFragment();
	}

	void RecordEnd()
	{
		if (m_hasActiveFragment)
			EndFragment();
		m_ended = true;
		m_hasActiveQuery = false;
		m_finishSubmission = m_renderer->GetRecordingSubmissionId();
		m_renderer->m_occlusionQueries.lastSubmission = m_finishSubmission;
		m_renderer->RequestSubmitSoon();
	}

	void BeginFragment()
	{
		HandleFinishedFragments();
		auto &oq = m_renderer->m_occlusionQueries;
		if (oq.freeIndices.empty())
		{
			cemuLog_log(LogType::Force, "D3D12: Exhausted occlusion query heap");
			return;
		}
		Fragment f;
		f.queryIndex = oq.freeIndices.back();
		oq.freeIndices.pop_back();
		m_renderer->GetCommandList()->BeginQuery(oq.heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, f.queryIndex);
		m_fragments.push_back(f);
		m_hasActiveFragment = true;
	}

	void EndFragment()
	{
		auto &oq = m_renderer->m_occlusionQueries;
		if (m_fragments.empty())
		{
			m_hasActiveFragment = false;
			return;
		}
		Fragment &f = m_fragments.back();
		auto *cmdList = m_renderer->GetCommandList();
		cmdList->EndQuery(oq.heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, f.queryIndex);
		cmdList->ResolveQueryData(oq.heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, f.queryIndex, 1, oq.resultBuffer.Get(), (UINT64)f.queryIndex * sizeof(uint64));
		f.submission = m_renderer->GetRecordingSubmissionId();
		f.finished = true;
		m_renderer->m_hasRecordedWork = true;
		m_hasActiveFragment = false;
	}

	void HandleFinishedFragments()
	{
		auto &oq = m_renderer->m_occlusionQueries;
		const uint64 completed = m_renderer->GetCompletedSubmissionId();
		while (!m_fragments.empty())
		{
			Fragment &f = m_fragments.front();
			if (!f.finished || f.submission > completed)
				break;
			m_accumulatedSum += oq.results[f.queryIndex];
			oq.freeIndices.push_back(f.queryIndex);
			m_fragments.erase(m_fragments.begin());
		}
	}

	void Reset()
	{
		queryEnded = false;
		queryEventStart = 0;
		queryEventEnd = 0;
		Dispatch(3);
	}
	void RecordReset()
	{
		auto &oq = m_renderer->m_occlusionQueries;
		for (auto &f : m_fragments)
			oq.freeIndices.push_back(f.queryIndex);
		m_fragments.clear();
		m_ended = false;
		m_hasActiveQuery = false;
		m_hasActiveFragment = false;
		m_accumulatedSum = 0;
		m_finishSubmission = 0;
	}

  private:
	struct Fragment
	{
		uint32 queryIndex = 0;
		uint64 submission = 0;
		bool finished = false;
	};
	D3D12Renderer *m_renderer;
	std::vector<Fragment> m_fragments;
	bool m_ended = false;
	bool m_hasActiveQuery = false;
	bool m_hasActiveFragment = false;
	uint64 m_finishSubmission = 0;
	uint64 m_accumulatedSum = 0;
	uint64 m_result = 0;
	bool m_resultReady = false;
};

LatteQueryObject *D3D12Renderer::occlusionQuery_create()
{
	auto &oq = m_occlusionQueries;
	BackendPacket packet{};
	packet.kind = BackendPacket::Kind::QueryHeap;
	ExecuteBackend(LattePipeline::Opcode::Create, packet);
	LatteProfiler::Add(LatteProfiler::Counter::ResourceCreateReplies);
	LatteQueryObjectD3D12 *query;
	if (oq.cached.empty())
		query = new LatteQueryObjectD3D12(this);
	else
	{
		query = oq.cached.back();
		oq.cached.pop_back();
	}
	query->recordingId = RegisterResource(query);
	query->Reset();
	query->Dispatch(5);
	return query;
}

void D3D12Renderer::occlusionQuery_destroy(LatteQueryObject *queryObj)
{
	auto *query = static_cast<LatteQueryObjectD3D12 *>(queryObj);
	auto &oq = m_occlusionQueries;
	query->Dispatch(4);
	query->Reset();
	query->Dispatch(6);
	RetireResource(query->recordingId);
	oq.cached.push_back(query);
}

void D3D12Renderer::occlusionQuery_flush_record()
{
	LatteProfiler::Add(LatteProfiler::Counter::QueryWaits);
	auto &oq = m_occlusionQueries;
	if (oq.lastSubmission > m_lastSubmittedFenceValue)
		SubmitCommandList(false);
	if (oq.lastSubmission)
		WaitForSubmission(oq.lastSubmission);
}

void D3D12Renderer::occlusionQuery_updateState_record()
{
	ProcessFinishedSubmissions();
}

void D3D12Renderer::occlusionQuery_notifyEndCommandList()
{
	for (auto *q : m_occlusionQueries.active)
		if (q->m_hasActiveQuery && q->m_hasActiveFragment)
			q->EndFragment();
}

void D3D12Renderer::occlusionQuery_notifyBeginCommandList()
{
	for (auto *q : m_occlusionQueries.active)
		if (q->m_hasActiveQuery)
			q->BeginFragment();
}

void D3D12Renderer::occlusionQuery_destroyAll()
{
	auto &oq = m_occlusionQueries;
	auto active = std::move(oq.active);
	oq.active.clear();
	for (auto *q : active)
	{
		RetireResource(q->recordingId);
		delete q;
	}
	for (auto *q : oq.cached)
		delete q;
	oq.active.clear();
	oq.cached.clear();
	if (oq.resultBuffer && oq.results)
		oq.resultBuffer->Unmap(0, nullptr);
	oq.results = nullptr;
}

void D3D12Renderer::query_record(const QueryPacket &packet, const LattePipeline::Arena &)
{
	m_pipeline.CheckBack();
	auto *query = m_resources.Get<LatteQueryObjectD3D12>(packet.query);
	switch (packet.action)
	{
	case 0:
		query->m_resultReady = query->RecordResult(query->m_result);
		break;
	case 1:
		query->RecordBegin();
		break;
	case 2:
		query->RecordEnd();
		break;
	case 3:
		query->RecordReset();
		break;
	case 4:
		if (query->m_hasActiveFragment)
			query->EndFragment();
		break;
	case 5:
		m_occlusionQueries.active.push_back(query);
		break;
	case 6:
		m_occlusionQueries.active.erase(std::remove(m_occlusionQueries.active.begin(), m_occlusionQueries.active.end(), query), m_occlusionQueries.active.end());
		break;
	default:
		throw std::logic_error("Invalid Latte query operation");
	}
	m_resources.Use(packet.query, m_operations.ActiveHeader().sequence, GetRecordingSubmissionId());
}
void D3D12Renderer::occlusionQuery_createHeap_record()
{
	auto &oq = m_occlusionQueries;
	if (!oq.heap)
	{
		D3D12_QUERY_HEAP_DESC desc{};
		desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
		desc.Count = kOcclusionQueryCount;
		D3D12_ThrowIfFailed(m_device->CreateQueryHeap(&desc, IID_PPV_ARGS(&oq.heap)), "CreateQueryHeap");
		D3D12_HEAP_PROPERTIES heap = D3D12_HeapProps(D3D12_HEAP_TYPE_READBACK);
		D3D12_RESOURCE_DESC bufferDesc = D3D12_BufferDesc((uint64)kOcclusionQueryCount * sizeof(uint64));
		D3D12_ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&oq.resultBuffer)), "CreateCommittedResource (query results)");
		void *mapped = nullptr;
		D3D12_ThrowIfFailed(oq.resultBuffer->Map(0, nullptr, &mapped), "Map (query results)");
		oq.results = (uint64 *)mapped;
		oq.freeIndices.reserve(kOcclusionQueryCount);
		for (uint32 i = kOcclusionQueryCount; i > 0; i--)
			oq.freeIndices.push_back(i - 1);
	}
}

#endif // ENABLE_D3D12
