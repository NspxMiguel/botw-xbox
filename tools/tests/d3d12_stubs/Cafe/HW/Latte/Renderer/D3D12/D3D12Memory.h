#pragma once
struct D3D12SubmissionTracker
{
	virtual ~D3D12SubmissionTracker() = default;
	virtual void WaitForSubmission(uint64 id) = 0;
	virtual uint64 GetCompletedSubmissionId() = 0;
};
