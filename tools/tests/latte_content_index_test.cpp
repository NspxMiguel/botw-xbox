#include "Cafe/HW/Latte/Core/LatteContentIndex.h"

#include <cassert>
#include <iostream>

int main()
{
	LatteDrawCache::ContentIndex<4> cache;
	assert(!cache.Find(0));
	cache.Store(0, 13);
	assert(*cache.Find(0) == 13);
	cache.Store(4, 19); // Collision replaces the hint; never aliases a fingerprint.
	assert(!cache.Find(0));
	assert(*cache.Find(4) == 19);
	cache.Clear();
	assert(!cache.Find(4));
	std::array<uint8_t, 67> bytes{};
	for (size_t size = 0; size <= bytes.size(); ++size)
	{
		const auto hash = LatteDrawCache::IndexContentHash(bytes.data(), size, 5, 2, 3);
		for (size_t i = 0; i < size; ++i)
		{
			bytes[i] = 1;
			bool sampled = size <= 32;
			if (!sampled)
				for (const auto offset : {size_t(0), size / 3, size * 2 / 3, size - sizeof(uint64_t)})
					sampled = sampled || (i >= offset && i < offset + sizeof(uint64_t));
			assert((LatteDrawCache::IndexContentHash(bytes.data(), size, 5, 2, 3) != hash) == sampled);
			bytes[i] = 0;
		}
		assert(LatteDrawCache::IndexContentHash(bytes.data(), size, 6, 2, 3) != hash);
		assert(LatteDrawCache::IndexContentHash(bytes.data(), size, 5, 3, 3) != hash);
		assert(LatteDrawCache::IndexContentHash(bytes.data(), size, 5, 2, 4) != hash);
	}
	std::cout << "Content index tests passed.\n";
}
