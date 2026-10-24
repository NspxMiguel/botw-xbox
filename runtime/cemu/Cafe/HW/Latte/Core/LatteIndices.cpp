#include "Cafe/HW/Latte/Core/LatteProfiler.h"
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/HW/Latte/Core/LatteDrawCache.h"
#include "Cafe/HW/Latte/Core/LatteContentIndex.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Common/cpu_features.h"
#include "Cafe/HW/Latte/Core/LatteIndexConversion.h"

#if defined(ARCH_X86_64)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace
{
constexpr size_t kPersistentEntries = LatteDrawCache::kSmallIndexEntries + LatteDrawCache::kLargeIndexEntries;
constexpr size_t kIndexWays = LatteDrawCache::kIndexCacheWays;
constexpr size_t kTransientEntries = 8;
} // namespace

struct
{
	struct CacheEntry
	{
		// input data
		const void* lastPtr;
		LatteDrawCache::IndexKey key;
		uint64 lastUsed;
		uint64 contentHash;
		size_t inputSize;
		LatteDrawCache::ContentSnapshot content;
		// output
		uint32 indexMax;
		uint32 rawIndexMax;
		uint32 restartIndex;
		Renderer::INDEX_TYPE renderIndexType;
		uint32 outputCount;
		Renderer::IndexAllocation indexAllocation;
	};
	// Separate small/large four-way banks retain more geometry without multiplying
	// the large-entry bound: at most 24 MiB input + 36 MiB output payload.
	std::array<CacheEntry, kPersistentEntries + kTransientEntries> entry;
	uint64 currentUsageCounter{0};
	LatteDrawCache::ContentIndex<> contentIndex;
	// Relocated addresses keep a non-owning hint. Full shape and byte validation
	// make stale hints safe even after slot eviction, mutation or invalidation.
	struct AddressHint
	{
		LatteDrawCache::IndexKey key{};
		size_t slot = 0;
		bool valid = false;
	};
	std::array<AddressHint, 4096> addressHints{};
}LatteIndexCache{};

void LatteIndices_invalidate(const void* memPtr, uint32 size)
{
	for(auto& entry : LatteIndexCache.entry)
	{
		if (entry.lastPtr && LatteDrawCache::RangesOverlap((uintptr_t)entry.lastPtr, entry.inputSize, (uintptr_t)memPtr, size))
		{
			if (entry.lastPtr)
				g_renderer->indexData_releaseIndexMemory(entry.indexAllocation);
			entry.lastPtr = nullptr;
		}
	}
}

void LatteIndices_invalidateOnWait()
{
	// D3D12 allocations remain owned by entries, independently of submissions. Hits
	// compare all guest bytes; other backends retain the original wait invalidation.
	if (g_renderer->GetType() != RendererAPI::D3D12)
	{
		LatteIndices_invalidateAll();
		return;
	}
	for (size_t i = kPersistentEntries; i < LatteIndexCache.entry.size(); ++i)
	{
		auto &entry = LatteIndexCache.entry[i];
		if (entry.lastPtr)
			g_renderer->indexData_releaseIndexMemory(entry.indexAllocation);
		entry.lastPtr = nullptr;
		entry.content.Clear();
	}
}

void LatteIndices_invalidateAll(Renderer *renderer)
{
	LatteIndexCache.contentIndex.Clear();
	for (auto &hint : LatteIndexCache.addressHints)
		hint.valid = false;
	if (!renderer)
		renderer = g_renderer.get();
	for(auto& entry : LatteIndexCache.entry)
	{
		if (entry.lastPtr)
			renderer->indexData_releaseIndexMemory(entry.indexAllocation);
		entry.content.Clear();
		entry.lastPtr = nullptr;
	}
}

uint64 LatteIndices_GetNextUsageIndex()
{
	return LatteIndexCache.currentUsageCounter++;
}

uint32 LatteIndices_calculateIndexOutputSize(LattePrimitiveMode primitiveMode, LatteIndexType indexType, uint32 count)
{
	if (primitiveMode == LattePrimitiveMode::QUADS)
	{
		sint32 numQuads = count / 4;
		if (indexType == LatteIndexType::AUTO)
		{
			if(count <= 0xFFFF)
				return numQuads * 6 * sizeof(uint16);
			return numQuads * 6 * sizeof(uint32);
		}
		if (indexType == LatteIndexType::U16_BE || indexType == LatteIndexType::U16_LE)
			return numQuads * 6 * sizeof(uint16);
		if (indexType == LatteIndexType::U32_BE || indexType == LatteIndexType::U32_LE)
			return numQuads * 6 * sizeof(uint32);
		cemu_assert_suspicious();
		return 0;
	}
	else if (primitiveMode == LattePrimitiveMode::QUAD_STRIP)
	{
		if (count <= 3)
		{
			return 0;
		}
		sint32 numQuads = (count-2) / 2;
		if (indexType == LatteIndexType::AUTO)
		{
			if (count <= 0xFFFF)
				return numQuads * 6 * sizeof(uint16);
			return numQuads * 6 * sizeof(uint32);
		}
		if (indexType == LatteIndexType::U16_BE || indexType == LatteIndexType::U16_LE)
			return numQuads * 6 * sizeof(uint16);
		if (indexType == LatteIndexType::U32_BE || indexType == LatteIndexType::U32_LE)
			return numQuads * 6 * sizeof(uint32);
		cemu_assert_suspicious();
		return 0;
	}
	else if (primitiveMode == LattePrimitiveMode::LINE_LOOP)
	{
		count++; // one extra vertex to reconnect the LINE_STRIP to the beginning
		if (indexType == LatteIndexType::AUTO)
		{
			if (count <= 0xFFFF)
				return count * sizeof(uint16);
			return count * sizeof(uint32);
		}
		if (indexType == LatteIndexType::U16_BE || indexType == LatteIndexType::U16_LE)
			return count * sizeof(uint16);
		if (indexType == LatteIndexType::U32_BE || indexType == LatteIndexType::U32_LE)
			return count * sizeof(uint32);
		cemu_assert_suspicious();
		return 0;
	}
	else if (primitiveMode == LattePrimitiveMode::TRIANGLE_FAN && (g_renderer->GetType() == RendererAPI::Metal || g_renderer->GetType() == RendererAPI::D3D12))
	{
		if (indexType == LatteIndexType::AUTO)
		{
			if (count <= 0xFFFF)
				return count * sizeof(uint16);
			return count * sizeof(uint32);
		}
		if (indexType == LatteIndexType::U16_BE || indexType == LatteIndexType::U16_LE)
			return count * sizeof(uint16);
		if (indexType == LatteIndexType::U32_BE || indexType == LatteIndexType::U32_LE)
			return count * sizeof(uint32);
		cemu_assert_suspicious();
		return 0;
	}
	else if(indexType == LatteIndexType::AUTO)
		return 0;
	else if (indexType == LatteIndexType::U16_BE || indexType == LatteIndexType::U16_LE)
		return count * sizeof(uint16);
	else if (indexType == LatteIndexType::U32_BE || indexType == LatteIndexType::U32_LE)
		return count * sizeof(uint32);
	return 0;
}

template <typename T>
void LatteIndices_convertBE(const void *input, void *output, uint32 count, uint32 &indexMax)
{
	LatteIndexConversion::CopyBigEndian<T>(input, output, count, indexMax);
}

template<typename T>
void LatteIndices_convertLE(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	const T* src = (T*)indexDataInput;
	T* dst = (T*)indexDataOutput;
	for (uint32 i = 0; i < count; i++)
	{
		T v = *src;
		*dst = v;
		indexMax = std::max(indexMax, (uint32)v);
		dst++;
		src++;
	}
}

template <typename T>
void LatteIndices_unpackQuadsAndConvert(const void *input, void *output, uint32 count, uint32 &indexMax)
{
	LatteIndexConversion::ExpandBigEndian<T, false>(input, output, count, indexMax);
}

template<typename T>
void LatteIndices_generateAutoQuadIndices(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	sint32 numQuads = count / 4;
	T* dst = (T*)indexDataOutput;
	for (sint32 i = 0; i < numQuads; i++)
	{
		T idx0 = i * 4 + 0;
		T idx1 = i * 4 + 1;
		T idx2 = i * 4 + 2;
		T idx3 = i * 4 + 3;
		dst[0] = idx0;
		dst[1] = idx1;
		dst[2] = idx2;
		dst[3] = idx0;
		dst[4] = idx2;
		dst[5] = idx3;
		dst += 6;
	}
	indexMax = std::max(count, 1u) - 1;
}

template <typename T>
void LatteIndices_unpackQuadStripAndConvert(const void *input, void *output, uint32 count, uint32 &indexMax)
{
	LatteIndexConversion::ExpandBigEndian<T, true>(input, output, count, indexMax);
}

template <typename T>
void LatteIndices_unpackLineLoopAndConvert(const void *input, void *output, uint32 count, uint32 &indexMax)
{
	if (!count)
		return;
	LatteIndexConversion::CopyBigEndian<T>(input, output, count, indexMax);
	// Read from guest memory, never from write-combined upload memory.
	T first;
	const auto *source = static_cast<const uint8 *>(input);
	first = 0;
	for (size_t byte = 0; byte < sizeof(T); ++byte)
		first = T((first << 8) | source[byte]);
	std::memcpy(static_cast<uint8 *>(output) + count * sizeof(T), &first, sizeof(T));
}

template<typename T>
void LatteIndices_generateAutoQuadStripIndices(void* indexDataOutput, uint32 count, uint32& indexMax)
{
	if (count <= 3)
		return;
	sint32 numQuads = (count - 2) / 2;
	T* dst = (T*)indexDataOutput;
	for (sint32 i = 0; i < numQuads; i++)
	{
		T idx0 = i * 2 + 0;
		T idx1 = i * 2 + 1;
		T idx2 = i * 2 + 2;
		T idx3 = i * 2 + 3;
		dst[0] = idx0;
		dst[1] = idx1;
		dst[2] = idx2;
		dst[3] = idx2;
		dst[4] = idx1;
		dst[5] = idx3;
		dst += 6;
	}
	indexMax = std::max(count, 1u) - 1;
}


template<typename T>
void LatteIndices_generateAutoLineLoopIndices(void* indexDataOutput, uint32 count, uint32& indexMax)
{
	if (count == 0)
		return;
	T* dst = (T*)indexDataOutput;
	for (sint32 i = 0; i < (sint32)count; i++)
	{
		*dst = (T)i;
		dst++;
	}
	*dst = 0;
	dst++;
	indexMax = std::max(count, 1u) - 1;
}

template<typename T>
void LatteIndices_unpackTriangleFanAndConvert(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	const betype<T>* src = (betype<T>*)indexDataInput;
	T* dst = (T*)indexDataOutput;
	// TODO: check this
	for (sint32 i = 0; i < count; i++)
	{
	    uint32 i0;
		if (i % 2 == 0)
		    i0 = i / 2;
        else
            i0 = count - 1 - i / 2;
        T idx = src[i0];
		indexMax = std::max(indexMax, (uint32)idx);
		dst[i] = idx;
	}
}

template<typename T>
void LatteIndices_generateAutoTriangleFanIndices(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	T* dst = (T*)indexDataOutput;
	for (sint32 i = 0; i < count; i++)
	{
		T idx = i;
		if (idx % 2 == 0)
            idx = idx / 2;
        else
            idx = count - 1 - idx / 2;
		dst[i] = idx;
	}
	indexMax = std::max(count, 1u) - 1;
}

#if defined(ARCH_X86_64)
ATTRIBUTE_AVX2
void LatteIndices_fastConvertU16_AVX2(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	// using AVX + AVX2 we can process 16 indices at a time
	const uint16* indicesU16BE = (const uint16*)indexDataInput;
	uint16* indexOutput = (uint16*)indexDataOutput;
	sint32 count16 = count >> 4;
	sint32 countRemaining = count & 15;
	if (count16)
	{
		__m256i mMin = _mm256_set_epi16((sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF,
						           		(sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF);
		__m256i mMax = _mm256_set_epi16(0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000);
		__m256i mShuffle16Swap = _mm256_set_epi8(30, 31, 28, 29, 26, 27, 24, 25, 22, 23, 20, 21, 18, 19, 16, 17, 14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);

		do
		{
			__m256i mIndexData = _mm256_loadu_si256((const __m256i*)indicesU16BE);
			indicesU16BE += 16;
			_mm_prefetch((const char*)indicesU16BE, _MM_HINT_T0);
			// endian swap
			mIndexData = _mm256_shuffle_epi8(mIndexData, mShuffle16Swap);
			_mm256_storeu_si256((__m256i *)indexOutput, mIndexData);
			mMax = _mm256_max_epu16(mIndexData, mMax);
			indexOutput += 16;
		} while (--count16);

		// fold 32 to 16 byte
		mMax = _mm256_max_epu16(mMax, _mm256_permute2x128_si256(mMax, mMax, 1));
		// fold 16 to 8 byte
		mMax = _mm256_max_epu16(mMax, _mm256_shuffle_epi32(mMax, (2 << 0) | (3 << 2) | (2 << 4) | (3 << 6)));

		uint16* mMaxU16 = (uint16*)&mMax;

		indexMax = std::max(indexMax, (uint32)mMaxU16[0]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[1]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[2]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[3]);
	}
	// process remaining indices
	uint32 _maxIndex = 0;
	for (sint32 i = countRemaining; (--i) >= 0;)
	{
		uint16 idx = _swapEndianU16(*indicesU16BE);
		*indexOutput = idx;
		indexOutput++;
		indicesU16BE++;
		_maxIndex = std::max(_maxIndex, (uint32)idx);
	}
	// update max
	indexMax = std::max(indexMax, _maxIndex);
}

ATTRIBUTE_SSE41
void LatteIndices_fastConvertU16_SSE41(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	// SSSE3 & SSE4.1 optimized decoding
	const uint16* indicesU16BE = (const uint16*)indexDataInput;
	uint16* indexOutput = (uint16*)indexDataOutput;
	sint32 count8 = count >> 3;
	sint32 countRemaining = count & 7;
	if (count8)
	{
		__m128i mMax = _mm_set_epi16(0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000);
		__m128i mTemp;
		__m128i* mRawIndices = (__m128i*)indicesU16BE;
		indicesU16BE += count8 * 8;
		__m128i* mOutputIndices = (__m128i*)indexOutput;
		indexOutput += count8 * 8;
		__m128i shufmask = _mm_set_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);
		while (count8--)
		{
			mTemp = _mm_loadu_si128(mRawIndices);
			mRawIndices++;
			mTemp = _mm_shuffle_epi8(mTemp, shufmask);
			mMax = _mm_max_epu16(mMax, mTemp);
			_mm_storeu_si128(mOutputIndices, mTemp);
			mOutputIndices++;
		}

		uint16* mMaxU16 = (uint16*)&mMax;

		indexMax = std::max(indexMax, (uint32)mMaxU16[0]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[1]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[2]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[3]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[4]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[5]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[6]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[7]);
	}
	uint32 _maxIndex = 0;
	for (sint32 i = countRemaining; (--i) >= 0;)
	{
		uint16 idx = _swapEndianU16(*indicesU16BE);
		*indexOutput = idx;
		indexOutput++;
		indicesU16BE++;
		_maxIndex = std::max(_maxIndex, (uint32)idx);
	}
	indexMax = std::max(indexMax, _maxIndex);
}

ATTRIBUTE_AVX2
void LatteIndices_fastConvertU32_AVX2(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	// using AVX + AVX2 we can process 8 indices at a time
	const uint32* indicesU32BE = (const uint32*)indexDataInput;
	uint32* indexOutput = (uint32*)indexDataOutput;
	sint32 count8 = count >> 3;
	sint32 countRemaining = count & 7;
	if (count8)
	{
		__m256i mMax = _mm256_set_epi32(0, 0, 0, 0, 0, 0, 0, 0);
		__m256i mShuffle32Swap = _mm256_set_epi8(28,29,30,31,
			24,25,26,27,
			20,21,22,23,
			16,17,18,19,
			12,13,14,15,
			8,9,10,11,
			4,5,6,7,
			0,1,2,3);
		// unaligned
		do
		{
			__m256i mIndexData = _mm256_loadu_si256((const __m256i*)indicesU32BE);
			indicesU32BE += 8;
			_mm_prefetch((const char*)indicesU32BE, _MM_HINT_T0);
			// endian swap
			mIndexData = _mm256_shuffle_epi8(mIndexData, mShuffle32Swap);
			_mm256_storeu_si256((__m256i *)indexOutput, mIndexData);
			mMax = _mm256_max_epu32(mIndexData, mMax);
			indexOutput += 8;
		} while (--count8);

		// fold 32 to 16 byte
		mMax = _mm256_max_epu32(mMax, _mm256_permute2x128_si256(mMax, mMax, 1));
		// fold 16 to 8 byte
		mMax = _mm256_max_epu32(mMax, _mm256_shuffle_epi32(mMax, (2 << 0) | (3 << 2) | (2 << 4) | (3 << 6)));

		uint32* mMaxU32 = (uint32*)&mMax;
		indexMax = std::max(indexMax, (uint32)mMaxU32[0]);
		indexMax = std::max(indexMax, (uint32)mMaxU32[1]);
	}
	// process remaining indices
	uint32 _maxIndex = 0;
	for (sint32 i = countRemaining; (--i) >= 0;)
	{
		uint32 idx = _swapEndianU32(*indicesU32BE);
		*indexOutput = idx;
		indexOutput++;
		indicesU32BE++;
		_maxIndex = std::max(_maxIndex, (uint32)idx);
	}
	// update min/max
	indexMax = std::max(indexMax, _maxIndex);
}
#elif defined(__aarch64__)

void LatteIndices_fastConvertU16_NEON(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	const uint16* indicesU16BE = (const uint16*)indexDataInput;
	uint16* indexOutput = (uint16*)indexDataOutput;
	sint32 count8 = count >> 3;
	sint32 countRemaining = count & 7;

	if (count8)
	{
		uint16x8_t mMax = vdupq_n_u16(0x0000);
		uint16x8_t mTemp;
		uint16x8_t* mRawIndices = (uint16x8_t*) indicesU16BE;
		indicesU16BE += count8 * 8;
		uint16x8_t* mOutputIndices = (uint16x8_t*) indexOutput;
		indexOutput += count8 * 8;

		while (count8--)
		{
			mTemp = vld1q_u16((uint16*)mRawIndices);
			mRawIndices++;
			mTemp = vreinterpretq_u16_u8(vrev16q_u8(vreinterpretq_u8_u16(mTemp)));
			mMax = vmaxq_u16(mMax, mTemp);
			vst1q_u16((uint16*)mOutputIndices, mTemp);
			mOutputIndices++;
		}

		uint16* mMaxU16 = (uint16*)&mMax;

		for (int i = 0; i < 8; ++i) {
			indexMax = std::max(indexMax, (uint32)mMaxU16[i]);
		}
	}
	// process remaining indices
	uint32 _maxIndex = 0;
	for (sint32 i = countRemaining; (--i) >= 0;)
	{
		uint16 idx = _swapEndianU16(*indicesU16BE);
		*indexOutput = idx;
		indexOutput++;
		indicesU16BE++;
		_maxIndex = std::max(_maxIndex, (uint32)idx);
	}
	// update min/max
	indexMax = std::max(indexMax, _maxIndex);
}

void LatteIndices_fastConvertU32_NEON(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	const uint32* indicesU32BE = (const uint32*)indexDataInput;
	uint32* indexOutput = (uint32*)indexDataOutput;
	sint32 count8 = count >> 2;
	sint32 countRemaining = count & 3;

	if (count8)
	{
		uint32x4_t mMax = vdupq_n_u32(0x00000000);
		uint32x4_t mTemp;
		uint32x4_t* mRawIndices = (uint32x4_t*) indicesU32BE;
		indicesU32BE += count8 * 4;
		uint32x4_t* mOutputIndices = (uint32x4_t*) indexOutput;
		indexOutput += count8 * 4;

		while (count8--)
		{
			mTemp = vld1q_u32((uint32*)mRawIndices);
			mRawIndices++;
			mTemp = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(mTemp)));
			mMax = vmaxq_u32(mMax, mTemp);
			vst1q_u32((uint32*)mOutputIndices, mTemp);
			mOutputIndices++;
		}

		uint32* mMaxU32 = (uint32*)&mMax;

		for (int i = 0; i < 4; ++i) {
			indexMax = std::max(indexMax, mMaxU32[i]);
		}
	}
	// process remaining indices
	uint32 _maxIndex = 0;
	for (sint32 i = countRemaining; (--i) >= 0;)
	{
		uint32 idx = _swapEndianU32(*indicesU32BE);
		*indexOutput = idx;
		indexOutput++;
		indicesU32BE++;
		_maxIndex = std::max(_maxIndex, idx);
	}
	// update min/max
	indexMax = std::max(indexMax, _maxIndex);
}

#endif

template <typename T, bool BigEndian = true>
void _LatteIndices_alternativeCalculateIndexMax(const void *indexData, uint32 count, uint32 primitiveRestartIndex, uint32 &indexMax)
{
	cemu_assert_debug(count != 0);
	const auto *idxPtrT = static_cast<const uint8 *>(indexData);
	T _indexMax = 0;
	cemu_assert_debug(primitiveRestartIndex <= std::numeric_limits<T>::max());
	T restartIndexT = (T)primitiveRestartIndex;
	while (count)
	{
		T idx;
		if constexpr (BigEndian)
			idx = *reinterpret_cast<const betype<T> *>(idxPtrT);
		else
			std::memcpy(&idx, idxPtrT, sizeof(T));
		if (idx != restartIndexT)
		{
			_indexMax = std::max(_indexMax, idx);
		}
		idxPtrT += sizeof(T);
		count--;
	}
	indexMax = _indexMax;
}

// calculate min and max index while taking primitive restart into account
// fallback implementation in case the fast path gives us invalid results
void LatteIndices_alternativeCalculateIndexMax(const void* indexData, LatteIndexType indexType, uint32 count, uint32& indexMax)
{
	if (count == 0)
	{
		indexMax = 0;
		return;
	}
	uint32 primitiveRestartIndex = LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.get_RESTART_INDEX();

	if (indexType == LatteIndexType::U16_BE)
	{
		_LatteIndices_alternativeCalculateIndexMax<uint16>(indexData, count, primitiveRestartIndex, indexMax);
	}
	else if (indexType == LatteIndexType::U32_BE)
	{
		_LatteIndices_alternativeCalculateIndexMax<uint32>(indexData, count, primitiveRestartIndex, indexMax);
	}
	else if (indexType == LatteIndexType::U16_LE)
		_LatteIndices_alternativeCalculateIndexMax<uint16, false>(indexData, count, primitiveRestartIndex, indexMax);
	else if (indexType == LatteIndexType::U32_LE)
		_LatteIndices_alternativeCalculateIndexMax<uint32, false>(indexData, count, primitiveRestartIndex, indexMax);
	// AUTO indices have no guest memory or primitive-restart token.
}

void LatteIndices_decode(const void* indexData, LatteIndexType indexType, uint32 count, LattePrimitiveMode primitiveMode, uint32& indexMax, Renderer::INDEX_TYPE& renderIndexType, uint32& outputCount, Renderer::IndexAllocation& indexAllocation)
{
	LatteProfiler::Scope profileScope(LatteProfiler::Slot::Indices);
	LatteProfiler::DetailScope lookupScope(LatteProfiler::Detail::IndexLookup);
	// what this should do:
	// [x] use fast SIMD-based index decoding
	// [x] unpack QUAD indices to triangle indices
	// [x] calculate min and max index, be careful about primitive restart index
	// [x] decode data directly into coherent memory buffer?
	// [x] bounded D3D12 reuse across frames with complete content validation

	const uint32 primitiveRestartIndex = LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.get_RESTART_INDEX();
	const size_t inputSize = indexType == LatteIndexType::AUTO ? 0 : size_t(count) * ((indexType == LatteIndexType::U16_BE || indexType == LatteIndexType::U16_LE) ? 2 : 4);
	const bool validateContent = g_renderer->GetType() == RendererAPI::D3D12;
	const uint32 indexOutputSize = LatteIndices_calculateIndexOutputSize(primitiveMode, indexType, count);
	if (indexOutputSize == 0)
	{
		outputCount = count;
		indexMax = std::max(count, 1u) - 1;
		renderIndexType = Renderer::INDEX_TYPE::NONE;
		indexAllocation = {};
		return;
	}
	const bool persistent = validateContent && inputSize <= 64 * 1024 && indexOutputSize <= 96 * 1024;
	// AUTO has no input bytes: its output is independent of the guest pointer.
	const LatteDrawCache::IndexKey key{indexType == LatteIndexType::AUTO ? 0 : (uintptr_t)indexData, count, (uint32)indexType, (uint32)primitiveMode, 0};
	auto begin = LatteIndexCache.entry.begin() + kPersistentEntries;
	auto end = LatteIndexCache.entry.end();
	if (persistent)
	{
		// Compare the complete key and all content bytes inside a four-way set.
		const bool small = inputSize <= LatteDrawCache::kSmallIndexInputLimit && indexOutputSize <= LatteDrawCache::kSmallIndexOutputLimit;
		const size_t entries = small ? LatteDrawCache::kSmallIndexEntries : LatteDrawCache::kLargeIndexEntries;
		const size_t slot = (small ? 0 : LatteDrawCache::kSmallIndexEntries) + key.Slot(entries / kIndexWays) * kIndexWays;
		begin = LatteIndexCache.entry.begin() + slot;
		end = begin + kIndexWays;
	}
	std::array<size_t, std::max(kIndexWays, kTransientEntries) + 1> rejectedSlots;
	size_t rejectedCount = 0;
	auto matchesContent = [&](const auto &entry, LatteProfiler::Counter path) {
		const auto comparison = entry.content.Compare(indexData, inputSize);
		LatteProfiler::Add(LatteProfiler::Counter::IndexComparedBytes, comparison.comparedBytes);
		LatteProfiler::Add(path, comparison.comparedBytes);
		if (comparison.sampleRejected)
			LatteProfiler::Add(LatteProfiler::Counter::IndexSampleRejects);
		return comparison.matches;
	};
	auto cacheEntry = std::find_if(begin, end, [&](const auto &entry) {
		if (!entry.lastPtr || entry.key != key)
			return false;
		if (!validateContent || matchesContent(entry, LatteProfiler::Counter::IndexAddressComparedBytes))
			return true;
		rejectedSlots[rejectedCount++] = &entry - LatteIndexCache.entry.data();
		return false;
	});
	bool cacheHit = cacheEntry != end;
	uint64 contentHash = 0;
	auto tryCandidate = [&](size_t slot) {
		// A stale address hint and the content directory can both name a bank
		// candidate already rejected above. Never compare the same bytes twice.
		if (std::find(rejectedSlots.begin(), rejectedSlots.begin() + rejectedCount, slot) != rejectedSlots.begin() + rejectedCount)
			return false;
		auto candidate = LatteIndexCache.entry.begin() + slot;
		if (!candidate->lastPtr || candidate->key.count != key.count || candidate->key.type != key.type || candidate->key.primitive != key.primitive)
			return false;
		if (!matchesContent(*candidate, LatteProfiler::Counter::IndexRelocatedComparedBytes))
		{
			if (rejectedCount < rejectedSlots.size())
				rejectedSlots[rejectedCount++] = slot;
			return false;
		}
		cacheEntry = candidate;
		LatteProfiler::Add(LatteProfiler::Counter::IndexRelocatedHits);
		return true;
	};
	if (!cacheHit && persistent && inputSize)
	{
		auto &hint = LatteIndexCache.addressHints[key.Slot(LatteIndexCache.addressHints.size())];
		if (hint.valid && hint.key == key)
			cacheHit = tryCandidate(hint.slot);
		if (!cacheHit)
		{
			contentHash = LatteDrawCache::IndexContentHash(indexData, inputSize, count, (uint32)indexType, (uint32)primitiveMode);
			LatteProfiler::Add(LatteProfiler::Counter::IndexHashedBytes, std::min<size_t>(inputSize, 32));
			if (const auto *slot = LatteIndexCache.contentIndex.Find(contentHash))
				cacheHit = tryCandidate(*slot);
		}
		if (cacheHit)
			hint = {key, size_t(cacheEntry - LatteIndexCache.entry.begin()), true};
	}
	if (cacheHit)
	{
		LatteProfiler::Add(LatteProfiler::Counter::IndexHits);
		// Restart changes the vertex bound, never the converted index bytes.
		// Recompute metadata without converting/uploading the same stream again.
		if (cacheEntry->restartIndex != primitiveRestartIndex)
		{
			cacheEntry->indexMax = cacheEntry->rawIndexMax;
			if (primitiveRestartIndex == cacheEntry->rawIndexMax && indexType != LatteIndexType::AUTO)
				LatteIndices_alternativeCalculateIndexMax(indexData, indexType, count, cacheEntry->indexMax);
			cacheEntry->restartIndex = primitiveRestartIndex;
		}
		indexMax = cacheEntry->indexMax;
		renderIndexType = cacheEntry->renderIndexType;
		outputCount = cacheEntry->outputCount;
		indexAllocation = cacheEntry->indexAllocation;
		cacheEntry->lastUsed = LatteIndices_GetNextUsageIndex();
		lookupScope.End(LatteProfiler::Detail::IndexHit);
		return;
	}
	LatteProfiler::Add(LatteProfiler::Counter::IndexMisses);
	if (!persistent)
		LatteProfiler::Add(LatteProfiler::Counter::IndexTransientMisses);
	else if (std::any_of(begin, end, [&](const auto &entry) { return entry.lastPtr && entry.key == key; }))
		LatteProfiler::Add(LatteProfiler::Counter::IndexContentMisses);
	else if (std::all_of(begin, end, [](const auto &entry) { return entry.lastPtr; }))
		LatteProfiler::Add(LatteProfiler::Counter::IndexSetEvictions);

	lookupScope.End(LatteProfiler::Detail::IndexMiss);
	LatteProfiler::DetailScope convertScope(LatteProfiler::Detail::IndexConvert);
	outputCount = 0;
	if (indexType == LatteIndexType::AUTO)
		renderIndexType = Renderer::INDEX_TYPE::NONE;
	else if (indexType == LatteIndexType::U16_BE || indexType == LatteIndexType::U16_LE)
		renderIndexType = Renderer::INDEX_TYPE::U16;
	else if (indexType == LatteIndexType::U32_BE || indexType == LatteIndexType::U32_LE)
		renderIndexType = Renderer::INDEX_TYPE::U32;
	else
		cemu_assert_debug(false);

	// query index buffer from renderer
	indexAllocation = g_renderer->indexData_reserveIndexMemory(indexOutputSize);
	void* indexOutputPtr = indexAllocation.mem;

	// decode indices
	indexMax = std::numeric_limits<uint32>::min();
	if (primitiveMode == LattePrimitiveMode::QUADS)
	{
		// unpack quads into triangles
		if (indexType == LatteIndexType::AUTO)
		{
			if (count <= 0xFFFF)
			{
				LatteIndices_generateAutoQuadIndices<uint16>(indexData, indexOutputPtr, count, indexMax);
				renderIndexType = Renderer::INDEX_TYPE::U16;
			}
			else
			{
				LatteIndices_generateAutoQuadIndices<uint32>(indexData, indexOutputPtr, count, indexMax);
				renderIndexType = Renderer::INDEX_TYPE::U32;
			}
		}
		else if (indexType == LatteIndexType::U16_BE)
			LatteIndices_unpackQuadsAndConvert<uint16>(indexData, indexOutputPtr, count, indexMax);
		else if (indexType == LatteIndexType::U32_BE)
			LatteIndices_unpackQuadsAndConvert<uint32>(indexData, indexOutputPtr, count, indexMax);
		else
			cemu_assert_debug(false);
		outputCount = count / 4 * 6;
	}
	else if (primitiveMode == LattePrimitiveMode::QUAD_STRIP)
	{
		// unpack quad strip into triangles
		if (indexType == LatteIndexType::AUTO)
		{
			if (count <= 0xFFFF)
			{
				LatteIndices_generateAutoQuadStripIndices<uint16>(indexOutputPtr, count, indexMax);
				renderIndexType = Renderer::INDEX_TYPE::U16;
			}
			else
			{
				LatteIndices_generateAutoQuadStripIndices<uint32>(indexOutputPtr, count, indexMax);
				renderIndexType = Renderer::INDEX_TYPE::U32;
			}
		}
		else if (indexType == LatteIndexType::U16_BE)
			LatteIndices_unpackQuadStripAndConvert<uint16>(indexData, indexOutputPtr, count, indexMax);
		else if (indexType == LatteIndexType::U32_BE)
			LatteIndices_unpackQuadStripAndConvert<uint32>(indexData, indexOutputPtr, count, indexMax);
		else
			cemu_assert_debug(false);
		if (count >= 2)
			outputCount = (count - 2) / 2 * 6;
		else
			outputCount = 0;
	}
	else if (primitiveMode == LattePrimitiveMode::LINE_LOOP)
	{
		// unpack line loop into line strip with extra reconnecting vertex
		if (indexType == LatteIndexType::AUTO)
		{
			if (count <= 0xFFFF)
			{
				LatteIndices_generateAutoLineLoopIndices<uint16>(indexOutputPtr, count, indexMax);
				renderIndexType = Renderer::INDEX_TYPE::U16;
			}
			else
			{
				LatteIndices_generateAutoLineLoopIndices<uint32>(indexOutputPtr, count, indexMax);
				renderIndexType = Renderer::INDEX_TYPE::U32;
			}
		}
		else if (indexType == LatteIndexType::U16_BE)
			LatteIndices_unpackLineLoopAndConvert<uint16>(indexData, indexOutputPtr, count, indexMax);
		else if (indexType == LatteIndexType::U32_BE)
			LatteIndices_unpackLineLoopAndConvert<uint32>(indexData, indexOutputPtr, count, indexMax);
		else
			cemu_assert_debug(false);
		outputCount = count + 1;
	}
	else if (primitiveMode == LattePrimitiveMode::TRIANGLE_FAN && (g_renderer->GetType() == RendererAPI::Metal || g_renderer->GetType() == RendererAPI::D3D12))
	{
        if (indexType == LatteIndexType::AUTO)
    	{
    		if (count <= 0xFFFF)
    		{
    			LatteIndices_generateAutoTriangleFanIndices<uint16>(indexData, indexOutputPtr, count, indexMax);
    			renderIndexType = Renderer::INDEX_TYPE::U16;
    		}
    		else
    		{
    			LatteIndices_generateAutoTriangleFanIndices<uint32>(indexData, indexOutputPtr, count, indexMax);
    			renderIndexType = Renderer::INDEX_TYPE::U32;
    		}
    	}
    	else if (indexType == LatteIndexType::U16_BE)
    		LatteIndices_unpackTriangleFanAndConvert<uint16>(indexData, indexOutputPtr, count, indexMax);
    	else if (indexType == LatteIndexType::U32_BE)
    		LatteIndices_unpackTriangleFanAndConvert<uint32>(indexData, indexOutputPtr, count, indexMax);
    	else
    		cemu_assert_debug(false);
    	outputCount = count;
	}
	else
	{
		if (indexType == LatteIndexType::U16_BE)
		{
#if defined(ARCH_X86_64)
			if (g_CPUFeatures.x86.avx2)
				LatteIndices_fastConvertU16_AVX2(indexData, indexOutputPtr, count, indexMax);
			else if (g_CPUFeatures.x86.sse4_1 && g_CPUFeatures.x86.ssse3)
				LatteIndices_fastConvertU16_SSE41(indexData, indexOutputPtr, count, indexMax);
			else
				LatteIndices_convertBE<uint16>(indexData, indexOutputPtr, count, indexMax);
#elif defined(__aarch64__)
			LatteIndices_fastConvertU16_NEON(indexData, indexOutputPtr, count, indexMax);
#else
			LatteIndices_convertBE<uint16>(indexData, indexOutputPtr, count, indexMax);
#endif
		}
		else if (indexType == LatteIndexType::U32_BE)
		{
#if defined(ARCH_X86_64)
			if (g_CPUFeatures.x86.avx2)
				LatteIndices_fastConvertU32_AVX2(indexData, indexOutputPtr, count, indexMax);
			else
				LatteIndices_convertBE<uint32>(indexData, indexOutputPtr, count, indexMax);
#elif defined(__aarch64__)
			LatteIndices_fastConvertU32_NEON(indexData, indexOutputPtr, count, indexMax);
#else
			LatteIndices_convertBE<uint32>(indexData, indexOutputPtr, count, indexMax);
#endif
		}
		else if (indexType == LatteIndexType::U16_LE)
		{
			LatteIndices_convertLE<uint16>(indexData, indexOutputPtr, count, indexMax);
		}
		else if (indexType == LatteIndexType::U32_LE)
		{
			LatteIndices_convertLE<uint32>(indexData, indexOutputPtr, count, indexMax);
		}
		else
			cemu_assert_debug(false);
		outputCount = count;
	}
	// the above algorithms use a fast approach to get indexMax which does not filter out indices matching primitiveRestartIndex
	// here we use a fallback in case the determined index equals the primitive restart index
	const uint32 rawIndexMax = indexMax;
	if (primitiveRestartIndex == indexMax && indexType != LatteIndexType::AUTO)
	{
		// recalculate index range but filter out primitive restart index
		LatteIndices_alternativeCalculateIndexMax(indexData, indexType, count, indexMax);
	}
	g_renderer->indexData_uploadIndexMemory(indexAllocation);
	performanceMonitor.cycle[performanceMonitor.cycleIndex].indexDataUploaded += indexOutputSize;
	// Content mutations replace their own key rather than consuming another way.
	auto lruEntry = std::find_if(begin, end, [&](const auto &entry) { return entry.lastPtr && entry.key == key; });
	if (lruEntry == end)
		lruEntry = std::find_if(begin, end, [](const auto &entry) { return !entry.lastPtr; });
	if (lruEntry == end)
		lruEntry = std::min_element(begin, end, [](const auto &a, const auto &b) { if (!a.lastPtr || !b.lastPtr)
			return !a.lastPtr && b.lastPtr;
		return a.lastUsed < b.lastUsed; });
	// invalidate previous allocation
	if (lruEntry->lastPtr)
		g_renderer->indexData_releaseIndexMemory(lruEntry->indexAllocation);
	// update cache
	if (validateContent)
		lruEntry->content.Assign(indexData, inputSize);
	lruEntry->contentHash = contentHash;
	if (persistent && inputSize)
		LatteIndexCache.contentIndex.Store(contentHash, size_t(lruEntry - LatteIndexCache.entry.begin()));
	lruEntry->inputSize = inputSize;
	lruEntry->key = key;
	lruEntry->lastPtr = indexType == LatteIndexType::AUTO ? reinterpret_cast<const void *>(uintptr_t(1)) : indexData;
	lruEntry->indexMax = indexMax;
	lruEntry->rawIndexMax = rawIndexMax;
	lruEntry->restartIndex = primitiveRestartIndex;
	lruEntry->renderIndexType = renderIndexType;
	lruEntry->outputCount = outputCount;
	lruEntry->indexAllocation = indexAllocation;
	lruEntry->lastUsed = LatteIndices_GetNextUsageIndex();
}
