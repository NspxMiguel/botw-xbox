#pragma once
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
struct D3D12SubmissionTracker {
  virtual ~D3D12SubmissionTracker() = default;
  virtual uint64 GetRecordingSubmissionId() const = 0;
  virtual void WaitForSubmission(uint64 id) = 0;
  virtual uint64 GetCompletedSubmissionId() = 0;
};
struct D3D12UploadAllocation {
  uint8 *cpuPtr;
  uint64 gpuAddress;
};
class D3D12UploadRing {
  struct Allocation {
    uint64 fence;
    std::unique_ptr<uint8[]> bytes;
  };
  D3D12SubmissionTracker *tracker;
  std::vector<Allocation> allocations;

public:
  D3D12UploadRing(ID3D12Device *, D3D12SubmissionTracker *t, uint64,
                  const char *)
      : tracker(t) {}
  D3D12UploadAllocation Allocate(uint64 size, uint64) {
    auto bytes = std::make_unique<uint8[]>(size);
    auto *ptr = bytes.get();
    allocations.push_back(
        {tracker->GetRecordingSubmissionId(), std::move(bytes)});
    return {ptr, reinterpret_cast<uint64>(ptr)};
  }
  void OnSubmit(uint64) {}
  void Retire(uint64 completed) {
    std::erase_if(allocations,
                  [completed](const auto &a) { return a.fence <= completed; });
  }
};
