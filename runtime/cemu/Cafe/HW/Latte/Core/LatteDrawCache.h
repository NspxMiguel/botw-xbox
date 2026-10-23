#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace LatteDrawCache
{
// Equality must compare values, never padding. Epochs describe the lifetime of the value.
template <typename Key, typename Value>
class ExactCache
{
  public:
	const Value *Find(const Key &key, uint64_t epoch = 0, uint64_t sourceEpoch = 0) const
	{
		return m_valid && m_epoch == epoch && m_sourceEpoch == sourceEpoch && m_key == key ? &m_value : nullptr;
	}
	void Store(const Key &key, const Value &value, uint64_t epoch = 0, uint64_t sourceEpoch = 0)
	{
		m_key = key;
		m_value = value;
		m_epoch = epoch;
		m_sourceEpoch = sourceEpoch;
		m_valid = true;
	}
	void Clear()
	{
		m_valid = false;
	}

  private:
	Key m_key{};
	Value m_value{};
	uint64_t m_epoch = 0;
	uint64_t m_sourceEpoch = 0;
	bool m_valid = false;
};

struct IndexKey
{
	uintptr_t address = 0;
	uint32_t count = 0;
	uint32_t type = 0;
	uint32_t primitive = 0;
	uint32_t restart = 0;
	bool operator==(const IndexKey &) const = default;
	size_t Slot(size_t capacity) const
	{
		// Fold high bits: guest addresses/counts are often aligned to large powers of two.
		uint64_t hash = (address >> 2) ^ (uint64_t(count) << 17) ^ (uint64_t(type) << 9) ^ primitive;
		hash = (hash ^ (hash >> 30)) * 0xBF58476D1CE4E5B9ull;
		hash = (hash ^ (hash >> 27)) * 0x94D049BB133111EBull;
		return (hash ^ (hash >> 31)) % capacity;
	}
};

// A complete CPU shadow detects middle-only writes too. Do not read write-combined uploads.
class ContentSnapshot
{
  public:
	bool Matches(const void *data, size_t size) const
	{
		return m_bytes.size() == size && (!size || std::memcmp(m_bytes.data(), data, size) == 0);
	}
	void Assign(const void *data, size_t size)
	{
		m_bytes.resize(size);
		if (size)
			std::memcpy(m_bytes.data(), data, size);
	}
	void Clear()
	{
		m_bytes.clear();
	}

  private:
	std::vector<uint8_t> m_bytes;
};

inline bool RangesOverlap(uintptr_t a, size_t aSize, uintptr_t b, size_t bSize)
{
	if (!aSize || !bSize)
		return false;
	return a <= b ? b - a < aSize : a - b < bSize;
}

// Raw loads expose a vectorizable endian-swap loop to clang-cl. Shadowed registers use
// the scalar PM4 path, since their guest-memory stores must retain packet ordering.
template <bool Compare>
bool StoreBigEndianWords(uint32_t *destination, const void *source, size_t count)
{
	const auto *bytes = static_cast<const uint8_t *>(source);
	uint32_t difference = 0;
	for (size_t i = 0; i < count; ++i)
	{
		uint32_t value;
		std::memcpy(&value, bytes + i * sizeof(value), sizeof(value));
		if constexpr (std::endian::native == std::endian::little)
			value = ((value & 0xFF) << 24) | ((value & 0xFF00) << 8) | ((value >> 8) & 0xFF00) | (value >> 24);
		if constexpr (Compare)
			difference |= destination[i] ^ value;
		destination[i] = value;
	}
	return difference != 0;
}
} // namespace LatteDrawCache
