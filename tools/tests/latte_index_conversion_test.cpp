#include "Cafe/HW/Latte/Core/LatteIndexConversion.h"

#include <cassert>
#include <iostream>
#include <limits>
#include <vector>

// Independent scalar specification, including exact output length and winding.
template <typename T, bool Strip>
static void CheckExpansion()
{
	for (uint32_t count = 0; count <= 131; ++count)
	{
		for (size_t misalignment = 0; misalignment < 32; ++misalignment)
		{
			const uint32_t quads = Strip ? (count < 4 ? 0 : (count - 2) / 2) : count / 4;
			std::vector<uint8_t> input(count * sizeof(T) + misalignment);
			std::vector<T> values(count);
			for (uint32_t i = 0; i < count; ++i)
			{
				values[i] = T((i * 0x98765431u) ^ (count * 0x1234567u));
				if (i == count / 2)
					values[i] = std::numeric_limits<T>::max();
				for (size_t byte = 0; byte < sizeof(T); ++byte)
					input[misalignment + i * sizeof(T) + byte] = uint8_t(values[i] >> (8 * (sizeof(T) - 1 - byte)));
			}
			std::vector<uint8_t> expected(quads * 6 * sizeof(T) + 64, 0xAD);
			uint32_t expectedMax = 7;
			for (uint32_t q = 0; q < quads; ++q)
			{
				const uint32_t base = q * (Strip ? 2 : 4);
				const unsigned order[] = {0, 1, 2, Strip ? 2u : 0u, Strip ? 1u : 2u, 3};
				for (unsigned v = 0; v < 4; ++v)
					expectedMax = std::max(expectedMax, uint32_t(values[base + v]));
				for (unsigned v = 0; v < 6; ++v)
					std::memcpy(expected.data() + misalignment + (q * 6 + v) * sizeof(T), &values[base + order[v]], sizeof(T));
			}
			std::vector<uint8_t> output(expected.size(), 0xAD);
			uint32_t maximum = 7;
			LatteIndexConversion::ExpandBigEndian<T, Strip>(input.data() + misalignment, output.data() + misalignment, count, maximum);
			assert(output == expected); // Prefix/suffix sentinels detect over-stores.
			assert(maximum == expectedMax);
		}
	}
}

template <typename T>
static void CheckCopy()
{
	for (uint32_t count = 0; count < 131; ++count)
	{
		for (size_t alignment = 0; alignment < 32; ++alignment)
		{
			std::vector<uint8_t> input(alignment + count * sizeof(T));
			std::vector<uint8_t> output(alignment + count * sizeof(T) + 32, 0xAD);
			auto expected = output;
			uint32_t expectedMax = 0;
			for (uint32_t i = 0; i < count; ++i)
			{
				const T value = T(i * 0x19713527u ^ count * 0x34579u);
				expectedMax = std::max(expectedMax, uint32_t(value));
				std::memcpy(expected.data() + alignment + i * sizeof(T), &value, sizeof(T));
				for (size_t byte = 0; byte < sizeof(T); ++byte)
					input[alignment + i * sizeof(T) + byte] = uint8_t(value >> (8 * (sizeof(T) - 1 - byte)));
			}
			uint32_t maximum = 0;
			LatteIndexConversion::CopyBigEndian<T>(input.data() + alignment, output.data() + alignment, count, maximum);
			assert(output == expected && maximum == expectedMax);
		}
	}
}

int main()
{
	CheckCopy<uint16_t>();
	CheckCopy<uint32_t>();
	CheckExpansion<uint16_t, false>();
	CheckExpansion<uint16_t, true>();
	CheckExpansion<uint32_t, false>();
	CheckExpansion<uint32_t, true>();
#if defined(__SSSE3__) && defined(__SSE4_1__)
	std::cout << "SIMD index expansion matches scalar output (all tails, widths, alignments, maxima).\n";
#else
	std::cout << "Portable index expansion matches scalar output (all tails, widths, alignments, maxima).\n";
#endif
}
