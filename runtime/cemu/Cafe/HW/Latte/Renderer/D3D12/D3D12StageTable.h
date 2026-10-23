// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Ported from PrismFox67/Cemu-UWP; see runtime/cemu/VENDORED.md.

#pragma once

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"

#include <array>

// Stage bindings are indexed by shader register. Cache keys remove root CBVs.
struct D3D12StageTableDesc
{
	D3D12_GPU_VIRTUAL_ADDRESS cbvAddress[D3D12Const::kMaxCBVsPerStage]{};
	uint32 cbvSize[D3D12Const::kMaxCBVsPerStage]{};
	D3D12_CPU_DESCRIPTOR_HANDLE srv[D3D12Const::kMaxSRVsPerStage]{};
	ID3D12Resource *uavResource = nullptr;
	uint32 uavSize = 0;
	D3D12StageTableDesc TableKey(D3D12Const::Stage stage) const
	{
		auto key = *this;
		for (uint32 i = 0; i < D3D12Const::RootCBVCount(stage); ++i)
		{
			key.cbvAddress[i] = 0;
			key.cbvSize[i] = 0;
		}
		return key;
	}
	uint64 Hash() const
	{
		uint64 hash = 0xCBF29CE484222325ull;
		auto mix = [&](uint64 value) { hash = (hash ^ value) * 0x100000001B3ull; };
		for (uint32 i = 0; i < D3D12Const::kMaxCBVsPerStage; ++i)
		{
			mix(cbvAddress[i]);
			mix(cbvSize[i]);
		}
		for (auto handle : srv)
			mix(handle.ptr);
		mix(reinterpret_cast<uintptr_t>(uavResource));
		mix(uavSize);
		return hash ^ (hash >> 32);
	}
	bool operator==(const D3D12StageTableDesc &other) const
	{
		return std::memcmp(cbvAddress, other.cbvAddress, sizeof(cbvAddress)) == 0 &&
			   std::memcmp(cbvSize, other.cbvSize, sizeof(cbvSize)) == 0 &&
			   std::memcmp(srv, other.srv, sizeof(srv)) == 0 && uavResource == other.uavResource && uavSize == other.uavSize;
	}
};

// Fixed open addressing: no per-draw allocation, exact equality after hashing.
// Epoch changes invalidate every slot in O(1). Full probe chains replace the home slot.
// At most 16 probes keep saturated-cache misses bounded; full-key comparisons
// preserve correctness on collisions and replacement.
template <size_t Capacity = 512>
class D3D12StageTableCache
{
	static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0);
	static constexpr size_t kProbeCount = Capacity < 16 ? Capacity : 16;

  public:
	const D3D12_GPU_DESCRIPTOR_HANDLE *Find(const D3D12StageTableDesc &key, uint64 generation, uint64 sourceGeneration)
	{
		if (generation != m_generation || sourceGeneration != m_sourceGeneration)
			return nullptr;
		const uint64 hash = key.Hash();
		for (size_t i = 0; i < kProbeCount; ++i)
		{
			const auto &entry = m_entries[(hash + i) & (Capacity - 1)];
			if (entry.epoch != m_epoch)
				return nullptr;
			if (entry.hash == hash && entry.key == key)
				return &entry.table;
		}
		return nullptr;
	}
	void Store(const D3D12StageTableDesc &key, D3D12_GPU_DESCRIPTOR_HANDLE table, uint64 generation, uint64 sourceGeneration)
	{
		if (generation != m_generation || sourceGeneration != m_sourceGeneration)
		{
			Clear();
			m_generation = generation;
			m_sourceGeneration = sourceGeneration;
		}
		const uint64 hash = key.Hash();
		size_t slot = hash & (Capacity - 1);
		for (size_t i = 0; i < kProbeCount; ++i)
		{
			const size_t candidate = (hash + i) & (Capacity - 1);
			const auto &entry = m_entries[candidate];
			if (entry.epoch != m_epoch || (entry.hash == hash && entry.key == key))
			{
				slot = candidate;
				break;
			}
		}
		m_entries[slot] = {key, table, hash, m_epoch};
	}
	void Clear()
	{
		if (++m_epoch == 0)
		{
			for (auto &entry : m_entries)
				entry.epoch = 0;
			m_epoch = 1;
		}
	}

  private:
	struct Entry
	{
		D3D12StageTableDesc key{};
		D3D12_GPU_DESCRIPTOR_HANDLE table{};
		uint64 hash = 0;
		uint64 epoch = 0;
	};
	std::array<Entry, Capacity> m_entries{};
	uint64 m_epoch = 1;
	uint64 m_generation = 0;
	uint64 m_sourceGeneration = 0;
};
