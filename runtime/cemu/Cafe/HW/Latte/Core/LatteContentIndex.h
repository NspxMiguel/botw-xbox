#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace LatteDrawCache
{
inline constexpr size_t kSmallIndexEntries = 2048;
inline constexpr size_t kLargeIndexEntries = 256;
inline constexpr size_t kIndexCacheWays = 4;
inline constexpr size_t kSmallIndexInputLimit = 4 * 1024;
inline constexpr size_t kSmallIndexOutputLimit = 6 * 1024;

// A hint into an owning cache, never an authority for equality or lifetime.
// The caller must validate the slot's live key and every input byte on a hit.
template <size_t Capacity = 4096>
class ContentIndex
{
	static_assert(Capacity && (Capacity & (Capacity - 1)) == 0);

  public:
	const size_t *Find(uint64_t hash) const
	{
		const auto &entry = m_entries[hash & (Capacity - 1)];
		return entry.valid && entry.hash == hash ? &entry.slot : nullptr;
	}
	void Store(uint64_t hash, size_t slot)
	{
		m_entries[hash & (Capacity - 1)] = {hash, slot, true};
	}
	void Clear()
	{
		for (auto &entry : m_entries)
			entry.valid = false;
	}

  private:
	struct Entry
	{
		uint64_t hash = 0;
		size_t slot = 0;
		bool valid = false;
	};
	std::array<Entry, Capacity> m_entries{};
};

// Full-input fingerprint used only after an address-cache miss. Equality always
// checks the complete CPU shadow too, including deliberate fingerprint collisions.
inline uint64_t IndexContentHash(const void *data, size_t bytes, uint32_t count, uint32_t type, uint32_t primitive)
{
	uint64_t hash = uint64_t(count) ^ (uint64_t(type) << 32) ^ (uint64_t(primitive) << 48);
	const auto *input = static_cast<const uint8_t *>(data);
	while (bytes >= sizeof(uint64_t))
	{
		uint64_t word;
		std::memcpy(&word, input, sizeof(word));
		hash = (hash ^ word) * 0x9E3779B185EBCA87ull;
		input += sizeof(word);
		bytes -= sizeof(word);
	}
	uint64_t tail = 0;
	if (bytes)
		std::memcpy(&tail, input, bytes);
	hash = (hash ^ tail) * 0xC2B2AE3D27D4EB4Full;
	hash ^= hash >> 33;
	return hash;
}
} // namespace LatteDrawCache
