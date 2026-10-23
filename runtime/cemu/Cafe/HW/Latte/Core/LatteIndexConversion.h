#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#if defined(__SSSE3__) && defined(__SSE4_1__)
#include <immintrin.h>
#endif

namespace LatteIndexConversion
{
template <typename T>
void CopyBigEndian(const void *input, void *output, uint32_t count, uint32_t &maximum)
{
	const auto *source = static_cast<const uint8_t *>(input);
	auto *destination = static_cast<uint8_t *>(output);
	uint32_t i = 0;
#if defined(__SSSE3__) && defined(__SSE4_1__)
	constexpr uint32_t width = 16 / sizeof(T);
	const auto swap = sizeof(T) == 2 ? _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14) : _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
	auto maxima = _mm_setzero_si128();
	for (; count - i >= width; i += width)
	{
		const auto values = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i *>(source + i * sizeof(T))), swap);
		_mm_storeu_si128(reinterpret_cast<__m128i *>(destination + i * sizeof(T)), values);
		if constexpr (sizeof(T) == 2)
			maxima = _mm_max_epu16(maxima, values);
		else
			maxima = _mm_max_epu32(maxima, values);
	}
	std::array<T, width> lanes;
	_mm_storeu_si128(reinterpret_cast<__m128i *>(lanes.data()), maxima);
	for (T value : lanes)
		maximum = std::max(maximum, uint32_t(value));
#endif
	for (; i < count; ++i)
	{
		T value = 0;
		for (size_t byte = 0; byte < sizeof(T); ++byte)
			value = T((value << 8) | source[i * sizeof(T) + byte]);
		std::memcpy(destination + i * sizeof(T), &value, sizeof(T));
		maximum = std::max(maximum, uint32_t(value));
	}
}

// QUADS and QUAD_STRIP share the four source vertices but have different winding.
// Decode and expand directly to the renderer allocation; never stage a converted array.
template <typename T, bool Strip>
void ExpandBigEndian(const void *input, void *output, uint32_t count, uint32_t &maximum)
{
	static_assert(sizeof(T) == 2 || sizeof(T) == 4);
	const auto *source = static_cast<const uint8_t *>(input);
	auto *destination = static_cast<uint8_t *>(output);
	const uint32_t quads = Strip ? (count < 4 ? 0 : (count - 2) / 2) : count / 4;
	constexpr std::array<unsigned, 6> order = Strip ? std::array<unsigned, 6>{0, 1, 2, 2, 1, 3} : std::array<unsigned, 6>{0, 1, 2, 0, 2, 3};
#if defined(__SSSE3__) && defined(__SSE4_1__)
	// One pshufb performs both endian swap and primitive expansion. Exact-width
	// loads/stores matter: a U16 quad is only eight bytes and its output is twelve.
	constexpr auto masks = [=] {
		std::array<std::array<uint8_t, 16>, 2> result{};
		for (auto &mask : result)
			mask.fill(0x80);
		for (unsigned i = 0; i < 6 * sizeof(T); ++i)
			result[i / 16][i % 16] = uint8_t(order[i / sizeof(T)] * sizeof(T) + sizeof(T) - 1 - i % sizeof(T));
		return result;
	}();
	const __m128i firstMask = _mm_loadu_si128(reinterpret_cast<const __m128i *>(masks[0].data()));
	const __m128i secondMask = _mm_loadu_si128(reinterpret_cast<const __m128i *>(masks[1].data()));
	__m128i maxima = _mm_setzero_si128();
	for (uint32_t i = 0; i < quads; ++i)
	{
		__m128i values;
		if constexpr (sizeof(T) == 2)
			values = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(source));
		else
			values = _mm_loadu_si128(reinterpret_cast<const __m128i *>(source));
		const __m128i first = _mm_shuffle_epi8(values, firstMask);
		if constexpr (sizeof(T) == 2)
		{
			_mm_storel_epi64(reinterpret_cast<__m128i *>(destination), first);
			const uint32_t tail = uint32_t(_mm_extract_epi32(first, 2));
			std::memcpy(destination + 8, &tail, sizeof(tail));
			maxima = _mm_max_epu16(maxima, first);
		}
		else
		{
			const __m128i second = _mm_shuffle_epi8(values, secondMask);
			_mm_storeu_si128(reinterpret_cast<__m128i *>(destination), first);
			_mm_storel_epi64(reinterpret_cast<__m128i *>(destination + 16), second);
			maxima = _mm_max_epu32(maxima, _mm_max_epu32(first, second));
		}
		source += (Strip ? 2 : 4) * sizeof(T);
		destination += 6 * sizeof(T);
	}
	std::array<T, 16 / sizeof(T)> lanes;
	_mm_storeu_si128(reinterpret_cast<__m128i *>(lanes.data()), maxima);
	for (T value : lanes)
		maximum = std::max(maximum, uint32_t(value));
#else
	for (uint32_t i = 0; i < quads; ++i)
	{
		std::array<T, 4> values;
		for (unsigned vertex = 0; vertex < 4; ++vertex)
		{
			T value = 0;
			for (unsigned byte = 0; byte < sizeof(T); ++byte)
				value = T((value << 8) | source[vertex * sizeof(T) + byte]);
			values[vertex] = value;
			maximum = std::max(maximum, uint32_t(value));
		}
		for (unsigned vertex = 0; vertex < 6; ++vertex)
			std::memcpy(destination + vertex * sizeof(T), &values[order[vertex]], sizeof(T));
		source += (Strip ? 2 : 4) * sizeof(T);
		destination += 6 * sizeof(T);
	}
#endif
}
} // namespace LatteIndexConversion
