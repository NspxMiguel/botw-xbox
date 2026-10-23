#pragma once

// Minimal device stand-in for testing production descriptor heap bookkeeping on a host without D3D12.
// It checks allocation/copy counts and handle reuse; it does not emulate rendering.
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
using uint8 = uint8_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using SIZE_T = size_t;
using UINT64 = uint64_t;
using UINT = unsigned;
using HRESULT = int;
namespace D3D12Const
{
constexpr uint32 kMaxSamplersPerStage = 18;
}
struct D3D12_CPU_DESCRIPTOR_HANDLE
{
	size_t ptr = 0;
};
struct D3D12_GPU_DESCRIPTOR_HANDLE
{
	uint64 ptr = 0;
};
enum D3D12_DESCRIPTOR_HEAP_TYPE
{
	D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
	D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
};
constexpr int D3D12_DESCRIPTOR_HEAP_FLAG_NONE = 0;
constexpr int D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE = 1;
struct D3D12_DESCRIPTOR_HEAP_DESC
{
	D3D12_DESCRIPTOR_HEAP_TYPE Type;
	uint32 NumDescriptors;
	int Flags;
};
struct D3D12_SAMPLER_DESC
{
	uint32 words[13]{};
};
struct ID3D12DescriptorHeap
{
	size_t cpu;
	uint64 gpu;
	D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandleForHeapStart()
	{
		return {cpu};
	}
	D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandleForHeapStart()
	{
		return {gpu};
	}
};
template <class T>
struct ComPtr
{
	T *ptr = nullptr;
	ComPtr() = default;
	~ComPtr()
	{
		delete ptr;
	}
	ComPtr(ComPtr &&other) noexcept : ptr(other.ptr)
	{
		other.ptr = nullptr;
	}
	ComPtr &operator=(ComPtr &&other) noexcept
	{
		delete ptr;
		ptr = other.ptr;
		other.ptr = nullptr;
		return *this;
	}
	T **operator&()
	{
		return &ptr;
	}
	T *operator->() const
	{
		return ptr;
	}
	T *Get() const
	{
		return ptr;
	}
};
struct ID3D12Device
{
	size_t next = 0x1000;
	uint32 copies = 0;
	uint32 GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE)
	{
		return 32;
	}
	HRESULT CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *, ID3D12DescriptorHeap **heap)
	{
		*heap = new ID3D12DescriptorHeap{next, next + 0x100000};
		next += 0x10000;
		return 0;
	}
	void CopyDescriptorsSimple(uint32 count, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE)
	{
		copies += count;
	}
	void CreateSampler(const D3D12_SAMPLER_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE)
	{
	}
};
#define IID_PPV_ARGS(p) p
#define cemu_assert(x) assert(x)
#define cemu_assert_debug(x) assert(x)
namespace fmt
{
template <class... T>
std::string format(const char *, T...)
{
	return {};
}
} // namespace fmt
namespace LogType
{
constexpr int Force = 0;
}
template <class... T>
void cemuLog_log(T...)
{
}
inline void D3D12_ThrowIfFailed(HRESULT hr, const char *)
{
	assert(hr == 0);
}
inline void D3D12_SetDebugName(ID3D12DescriptorHeap *, const std::string &)
{
}
