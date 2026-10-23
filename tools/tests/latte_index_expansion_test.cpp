#include "Cafe/HW/Latte/Core/LatteIndexConversion.h"
#include <cassert>
#include <iostream>
#include <random>
#include <vector>

template <typename T, bool Strip>
void Check()
{
	std::mt19937 random(0x137);
	for (unsigned count = 0; count < 300; ++count)
	{
		const unsigned quads =
			Strip ? (count < 4 ? 0 : (count - 2) / 2) : count / 4;
		// Unaligned source and destination with canaries around the exact output
		// span.
		std::vector<uint8_t> input(count * sizeof(T) + 1);
		for (auto &byte : input)
			byte = uint8_t(random());
		std::vector<uint8_t> output(quads * 6 * sizeof(T) + 2, 0xCD);
		std::vector<uint8_t> reference = output;
		uint32_t expectedMax = 7, actualMax = 7;
		for (unsigned i = 0; i < quads; ++i)
		{
			T values[4];
			for (unsigned j = 0; j < 4; ++j)
			{
				T value = 0;
				for (unsigned b = 0; b < sizeof(T); ++b)
					value = T((value << 8) |
							  input[1 + ((Strip ? 2 : 4) * i + j) * sizeof(T) + b]);
				values[j] = value;
				expectedMax = std::max(expectedMax, uint32_t(value));
			}
			unsigned order[] = {0, 1, 2, Strip ? 2u : 0u, Strip ? 1u : 2u, 3};
			for (unsigned j = 0; j < 6; ++j)
				std::memcpy(reference.data() + 1 + (i * 6 + j) * sizeof(T),
							&values[order[j]], sizeof(T));
		}
		LatteIndexConversion::ExpandBigEndian<T, Strip>(
			input.data() + 1, output.data() + 1, count, actualMax);
		assert(output == reference && expectedMax == actualMax);
	}
}
int main()
{
	Check<uint16_t, false>();
	Check<uint16_t, true>();
	Check<uint32_t, false>();
	Check<uint32_t, true>();
	std::cout << "Exact expansion probe passed.\n";
}
