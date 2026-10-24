#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

// One consumer owns these copied words. Producers only access the original
// queue under its mutex. Prefetching does not execute packets or retire work.
template <size_t Capacity = 64>
class LatteWordBatch
{
	static_assert(Capacity > 0);

  public:
	enum class Result
	{
		Empty,
		Buffered,
		Refilled,
	};
	bool HasBufferedWords() const
	{
		return m_next != m_count;
	}
	template <typename Queue, typename Mutex>
	Result Read(Queue &queue, Mutex &mutex, uint32_t &word)
	{
		if (HasBufferedWords())
		{
			word = m_words[m_next++];
			return Result::Buffered;
		}
		std::lock_guard lock(mutex);
		m_next = 0;
		m_count = std::min(queue.size(), Capacity);
		for (size_t i = 0; i < m_count; ++i)
		{
			m_words[i] = queue.front();
			queue.pop_front();
		}
		if (!m_count)
			return Result::Empty;
		word = m_words[m_next++];
		return Result::Refilled;
	}

  private:
	std::array<uint32_t, Capacity> m_words{};
	size_t m_next = 0;
	size_t m_count = 0;
};
