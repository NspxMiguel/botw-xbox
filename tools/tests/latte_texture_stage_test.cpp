#include "Cafe/HW/Latte/Renderer/D3D12/D3D12TextureStage.h"

#include <cassert>
#include <iostream>

int main()
{
	D3D12TextureStageCache cache;
	D3D12TextureStageKey key{};
	key.shaderId = 1;
	key.count = 2;
	key.units[0].viewId = 2;
	key.units[1].viewId = 3;
	D3D12TextureStageValue value{};
	value.srv[0].ptr = 42;
	value.samplerCount = 2;
	assert(!cache.Find(key, 1, 1));
	cache.Store(key, value, 1, 1);
	assert(cache.Find(key, 1, 1)->srv[0].ptr == 42);
	assert(!cache.Find(key, 2, 1)); // Lookup insertion/removal.
	assert(!cache.Find(key, 1, 2)); // Recycled/rewritten source descriptors.
	for (size_t unit = 0; unit < key.count; ++unit)
	{
		for (size_t word = 0; word < key.units[unit].words.size(); ++word)
		{
			auto changed = key;
			++changed.units[unit].words[word];
			assert(!cache.Find(changed, 1, 1));
		}
		auto changed = key;
		++changed.units[unit].viewId;
		assert(!cache.Find(changed, 1, 1));
	}
	auto changed = key;
	++changed.shaderId; // Includes immutable resource remap, dimension and depth use.
	assert(!cache.Find(changed, 1, 1));
	changed = key;
	--changed.count;
	assert(!cache.Find(changed, 1, 1));
	// Inactive slots do not contribute, even if a previous shader used them.
	changed = key;
	changed.units[2].viewId = 99;
	assert(cache.Find(changed, 1, 1));
	auto alternating = key;
	alternating.shaderId = 2;
	cache.Store(alternating, value, 1, 1);
	assert(cache.Find(key, 1, 1));
	assert(cache.Find(alternating, 1, 1));
	alternating.shaderId = key.shaderId + 32;
	cache.Store(alternating, value, 1, 1);
	assert(!cache.Find(key, 1, 1)); // Direct-map replacement cannot alias a shader.
	assert(cache.Find(alternating, 1, 1));
	cache.Clear();
	assert(!cache.Find(alternating, 1, 1));
	assert(!cache.Find(key, 1, 1));
	key.count = 0;
	cache.Store(key, value, 3, 4);
	assert(cache.Find(key, 3, 4));
	std::cout << "Texture stage cache tests passed.\n";
}
