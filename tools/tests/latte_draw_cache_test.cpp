#include "Cafe/HW/Latte/Core/LatteDrawCache.h"

#include <cassert>
#include <iostream>

using namespace LatteDrawCache;

int main()
{
	// Table contents are equal but their GPU allocation and source descriptor lifetimes differ.
	ExactCache<std::array<uint64_t, 54>, uint64_t> tables;
	std::array<uint64_t, 54> descriptors{};
	descriptors[0] = 0x10000;
	assert(!tables.Find(descriptors, 1, 1));
	tables.Store(descriptors, 42, 1, 1);
	assert(*tables.Find(descriptors, 1, 1) == 42);
	assert(!tables.Find(descriptors, 2, 1)); // submission/wrap/retirement
	assert(!tables.Find(descriptors, 1, 2)); // staging slot recycling
	for (size_t i = 0; i < descriptors.size(); ++i)
	{
		auto changed = descriptors;
		changed[i] ^= 1;
		assert(!tables.Find(changed, 1, 1));
	}
	tables.Clear();
	assert(!tables.Find(descriptors, 1, 1));

	// Identical transition requests stop hitting after an overlapping or disjoint state write.
	ExactCache<std::array<uint32_t, 5>, bool> transition;
	const std::array<uint32_t, 5> range{0, 4, 1, 3, 16};
	transition.Store(range, true);
	assert(transition.Find(range));
	for (size_t i = 0; i < range.size(); ++i)
	{
		auto changed = range;
		++changed[i];
		assert(!transition.Find(changed));
	}
	transition.Clear();
	assert(!transition.Find(range));

	// Immutable views memoize a resource version. Overlapping/disjoint state
	// changes invalidate every view; a content write alone does not change state.
	VersionMemo viewA, viewB;
	uint64_t version = 1;
	assert(!viewA.Matches(version));
	viewA.Remember(version);
	viewB.Remember(version);
	assert(viewA.Matches(version) && viewB.Matches(version));
	++version;
	assert(!viewA.Matches(version) && !viewB.Matches(version));
	viewA.Remember(version);
	assert(viewA.Matches(version) && !viewB.Matches(version));

	// A shader-state node remains valid across map growth, but must be cleared
	// before removal/unload even when the hash key would otherwise match.
	ExactCache<uint64_t, const void *> shaderState;
	int firstNode = 0;
	shaderState.Store(17, &firstNode);
	assert(*shaderState.Find(17) == &firstNode);
	assert(!shaderState.Find(18));
	shaderState.Clear();
	assert(!shaderState.Find(17));

	// Partial invalidation must intersect the complete input span, including writes
	// that start before the cached address and finish inside it.
	assert(RangesOverlap(100, 200, 50, 100));
	assert(RangesOverlap(100, 200, 299, 1));
	assert(!RangesOverlap(100, 200, 300, 1));
	assert(!RangesOverlap(100, 200, 0, 100));
	assert(!RangesOverlap(100, 0, 100, 1));
	assert(RangesOverlap(UINTPTR_MAX - 8, 8, UINTPTR_MAX - 1, 1));

	const IndexKey indexKey{0x100000, 512, 1, 4, 0xFFFF};
	ExactCache<IndexKey, uint64_t> indexEntry;
	indexEntry.Store(indexKey, 73);
	assert(*indexEntry.Find(indexKey) == 73);
	for (const auto &changed : {IndexKey{0x100004, 512, 1, 4, 0xFFFF}, IndexKey{0x100000, 511, 1, 4, 0xFFFF},
								IndexKey{0x100000, 512, 2, 4, 0xFFFF}, IndexKey{0x100000, 512, 1, 5, 0xFFFF}, IndexKey{0x100000, 512, 1, 4, 0xFFFE}})
		assert(!indexEntry.Find(changed));
	// Two complete keys in one bucket must replace rather than alias the old allocation.
	auto collision = indexKey;
	do
	{
		++collision.address;
	} while (collision.Slot(64) != indexKey.Slot(64));
	assert(!indexEntry.Find(collision));
	indexEntry.Store(collision, 74);
	assert(!indexEntry.Find(indexKey));
	assert(*indexEntry.Find(collision) == 74);

	ContentSnapshot snapshot;
	std::array<uint8_t, 1024> indices{};
	for (size_t i = 0; i < indices.size(); ++i)
		indices[i] = uint8_t(i);
	snapshot.Assign(indices.data(), indices.size());
	assert(snapshot.Matches(indices.data(), indices.size()));
	for (size_t i = 0; i < indices.size(); ++i)
	{
		indices[i] ^= 1;
		const auto result = snapshot.Compare(indices.data(), indices.size());
		assert(!result.matches);
		if (i == indices.size() - 1)
			assert(result.sampleRejected && result.comparedBytes == 32);
		if (i == 511)
			assert(!result.sampleRejected && result.comparedBytes == indices.size());
		indices[i] ^= 1;
	}
	assert(!snapshot.Matches(indices.data(), indices.size() - 1));
	snapshot.Clear();
	assert(!snapshot.Matches(indices.data(), indices.size()));
	snapshot.Assign(nullptr, 0); // auto indices have no guest input bytes
	assert(snapshot.Matches(nullptr, 0));

	// Includes unaligned input and vector-sized ranges with a scalar tail.
	std::array<uint8_t, 4 * 67 + 1> packet{};
	for (size_t i = 0; i < 67; ++i)
	{
		packet[i * 4 + 1] = uint8_t(i);
		packet[i * 4 + 2] = uint8_t(i + 1);
		packet[i * 4 + 3] = uint8_t(i + 2);
		packet[i * 4 + 4] = uint8_t(i + 3);
	}
	for (size_t count = 0; count <= 67; ++count)
	{
		std::array<uint32_t, 68> output{};
		output.fill(0xDEADBEEF);
		assert(!StoreBigEndianWords<false>(output.data(), packet.data() + 1, count));
		assert(output[count] == 0xDEADBEEF);
		for (size_t i = 0; i < count; ++i)
			assert(output[i] == ((i << 24) | ((i + 1) << 16) | ((i + 2) << 8) | (i + 3)));
		assert(!StoreBigEndianWords<true>(output.data(), packet.data() + 1, count));
		if (count)
		{
			output[count / 2] ^= 1;
			assert(StoreBigEndianWords<true>(output.data(), packet.data() + 1, count));
			assert(!StoreBigEndianWords<true>(output.data(), packet.data() + 1, count));
		}
	}
	std::cout << "Draw cache tests passed (exact keys, epochs, content, overlaps, PM4 bulk stores).\n";
}
