#pragma once

#include "Cafe/HW/Latte/Core/LatteDrawCache.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"

// All mutable inputs to SRV selection and sampler translation for one shader.
// Only the first count units are initialized/read. No native handles or pointers
// establish object identity: shader/view IDs never repeat after destruction.
struct D3D12TextureStageKey
{
	struct Unit
	{
		std::array<uint32, 19> words; // texture[7], sampler[3], border[4], overrides[5]
		uint64 viewId;
		bool operator==(const Unit &other) const
		{
			return viewId == other.viewId && words == other.words;
		}
	};
	uint64 shaderId;
	uint32 count;
	std::array<Unit, D3D12Const::kMaxSRVsPerStage> units;
	D3D12TextureStageKey() = default;
	D3D12TextureStageKey(const D3D12TextureStageKey &other)
	{
		*this = other;
	}
	D3D12TextureStageKey &operator=(const D3D12TextureStageKey &other)
	{
		shaderId = other.shaderId;
		count = other.count;
		for (uint32 i = 0; i < count; ++i)
			units[i] = other.units[i];
		return *this;
	}
	bool operator==(const D3D12TextureStageKey &other) const
	{
		if (shaderId != other.shaderId || count != other.count)
			return false;
		for (uint32 i = 0; i < count; ++i)
			if (!(units[i] == other.units[i]))
				return false;
		return true;
	}
};

struct D3D12TextureStageValue
{
	D3D12_CPU_DESCRIPTOR_HANDLE srv[D3D12Const::kMaxSRVsPerStage]{};
	D3D12_SAMPLER_DESC samplers[D3D12Const::kMaxSamplersPerStage];
	uint32 samplerCount;
};

// Retain the last inputs for several alternating shaders, not just adjacent draws.
// Direct-map collisions replace an entry; complete values still establish hits.
class D3D12TextureStageCache
{
  public:
	const D3D12TextureStageValue *Find(const D3D12TextureStageKey &key, uint64 textureGeneration, uint64 sourceGeneration) const
	{
		return m_entries[key.shaderId % m_entries.size()].Find(key, textureGeneration, sourceGeneration);
	}
	void Store(const D3D12TextureStageKey &key, const D3D12TextureStageValue &value, uint64 textureGeneration, uint64 sourceGeneration)
	{
		m_entries[key.shaderId % m_entries.size()].Store(key, value, textureGeneration, sourceGeneration);
	}
	void Clear()
	{
		for (auto &entry : m_entries)
			entry.Clear();
	}

  private:
	std::array<LatteDrawCache::ExactCache<D3D12TextureStageKey, D3D12TextureStageValue>, 32> m_entries;
};
