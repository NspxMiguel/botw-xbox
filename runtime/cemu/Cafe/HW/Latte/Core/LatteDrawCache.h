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

// A view is immutable and owns its memo. Any base-resource state mutation
// advances the version, invalidating all views without walking a view list.
class VersionMemo
{
  public:
	bool Matches(uint64_t version) const
	{
		return m_version == version;
	}
	void Remember(uint64_t version)
	{
		m_version = version;
	}

  private:
	uint64_t m_version = 0; // Resource versions start at one.
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
	struct Comparison
	{
		bool matches;
		size_t comparedBytes;
		bool sampleRejected = false;
	};
	Comparison Compare(const void *data, size_t size) const
	{
		if (m_bytes.size() != size)
			return {false, 0};
		if (size >= kSampleThreshold)
		{
			const auto samples = Sample(data, size);
			if (samples != m_samples)
				return {false, sizeof(samples), true};
		}
		// Samples only reject candidates. Matching samples still verify every
		// byte: guest stores have no comprehensive write-generation authority.
		return {!size || std::memcmp(m_bytes.data(), data, size) == 0, size};
	}
	bool Matches(const void *data, size_t size) const
	{
		return Compare(data, size).matches;
	}
	void Assign(const void *data, size_t size)
	{
		m_bytes.resize(size);
		if (size)
			std::memcpy(m_bytes.data(), data, size);
		if (size >= kSampleThreshold)
			m_samples = Sample(m_bytes.data(), size);
	}
	void Clear()
	{
		m_bytes.clear();
	}

  private:
	static constexpr size_t kSampleThreshold = 256;
	static std::array<uint64_t, 4> Sample(const void *data, size_t size)
	{
		const auto *bytes = static_cast<const uint8_t *>(data);
		std::array<uint64_t, 4> samples;
		const std::array<size_t, 4> offsets{0, size / 3, size * 2 / 3, size - sizeof(uint64_t)};
		for (size_t i = 0; i < offsets.size(); ++i)
			std::memcpy(&samples[i], bytes + offsets[i], sizeof(uint64_t));
		return samples;
	}
	std::array<uint64_t, 4> m_samples{};
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
	auto store = [&](size_t i) {
		uint32_t value;
		std::memcpy(&value, bytes + i * sizeof(value), sizeof(value));
		if constexpr (std::endian::native == std::endian::little)
			value = ((value & 0xFF) << 24) | ((value & 0xFF00) << 8) | ((value >> 8) & 0xFF00) | (value >> 24);
		if constexpr (Compare)
			difference |= destination[i] ^ value;
		destination[i] = value;
	};
	// Most PM4 register packets carry two to four words. Avoid the general
	// vector-loop setup and tail checks on that path.
	if (count <= 4)
	{
		switch (count)
		{
		case 4:
			store(3);
			[[fallthrough]];
		case 3:
			store(2);
			[[fallthrough]];
		case 2:
			store(1);
			[[fallthrough]];
		case 1:
			store(0);
			[[fallthrough]];
		default:
			break;
		}
	}
	else
		for (size_t i = 0; i < count; ++i)
			store(i);
	return difference != 0;
}
} // namespace LatteDrawCache
