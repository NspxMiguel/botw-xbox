#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using sint16 = int16_t;
using sint32 = int32_t;
#define cemu_assert_debug(expression) assert(expression)
#define cemu_assert_suspicious() assert(false)

inline uint16 _swapEndianU16(uint16 value)
{
	return __builtin_bswap16(value);
}
inline uint32 _swapEndianU32(uint32 value)
{
	return __builtin_bswap32(value);
}

template <typename T>
struct betype
{
	T raw;
	operator T() const
	{
		if constexpr (sizeof(T) == 2)
			return _swapEndianU16(raw);
		else
			return _swapEndianU32(raw);
	}
};

enum class LatteIndexType
{
	AUTO,
	U16_BE,
	U16_LE,
	U32_BE,
	U32_LE
};
enum class LattePrimitiveMode
{
	TRIANGLES,
	QUADS,
	QUAD_STRIP,
	LINE_LOOP,
	TRIANGLE_FAN
};
enum class RendererAPI
{
	D3D12,
	Metal,
	Vulkan
};

struct Renderer
{
	enum class INDEX_TYPE
	{
		NONE,
		U16,
		U32
	};
	struct IndexAllocation
	{
		void *mem;
		void *rendererInternal;
	};
	struct Allocation
	{
		std::vector<uint8> bytes;
	};
	RendererAPI api = RendererAPI::D3D12;
	uint32 uploads = 0;
	uint32 releases = 0;
	uint32 live = 0;
	RendererAPI GetType() const
	{
		return api;
	}
	IndexAllocation indexData_reserveIndexMemory(uint32 size)
	{
		auto *allocation = new Allocation;
		allocation->bytes.resize(size);
		++live;
		return {allocation->bytes.data(), allocation};
	}
	void indexData_releaseIndexMemory(IndexAllocation &allocation)
	{
		assert(allocation.rendererInternal);
		delete static_cast<Allocation *>(allocation.rendererInternal);
		allocation = {};
		++releases;
		--live;
	}
	void indexData_uploadIndexMemory(IndexAllocation &)
	{
		++uploads;
	}
};

inline std::unique_ptr<Renderer> g_renderer;
struct TestGPUState
{
	struct
	{
		struct
		{
			uint32 value = UINT32_MAX;
			uint32 get_RESTART_INDEX() const
			{
				return value;
			}
		} VGT_MULTI_PRIM_IB_RESET_INDX;
	} contextNew;
};
inline TestGPUState LatteGPUState;
struct TestPerformanceMonitor
{
	struct
	{
		uint64 indexDataUploaded = 0;
	} cycle[1];
	uint32 cycleIndex = 0;
};
inline TestPerformanceMonitor performanceMonitor;
