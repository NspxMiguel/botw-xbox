#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace LattePipeline
{
struct ChunkRange
{
	size_t begin = 0, end = 0;
};
struct ChunkTiming
{
	std::array<uint64_t, 4> busyNs{};
	uint64_t criticalPathNs = 0, uniformUploadBytes = 0;
	unsigned count = 0;
};

// A coordinator owns this pool. Run joins every job before returning (including
// on failure), so immutable input leases and native submission stay outside it.
class ChunkWorkers
{
  public:
	static constexpr unsigned MaxWorkers = 4;
	static constexpr size_t DrawsPerChunk = 512;
	static constexpr size_t MaxDraws = DrawsPerChunk * MaxWorkers;

	static unsigned Count(size_t draws, unsigned requested = 0)
	{
		if (!draws)
			return 0;
		return static_cast<unsigned>(std::min<size_t>(draws, std::clamp<unsigned>(requested ? requested : static_cast<unsigned>((draws + DrawsPerChunk - 1) / DrawsPerChunk), 1, MaxWorkers)));
	}
	static ChunkRange Range(size_t draws, unsigned count, unsigned worker)
	{
		if (!count || count > MaxWorkers || worker >= count)
			throw std::invalid_argument("Invalid Latte chunk range");
		return {draws * worker / count, draws * (worker + 1) / count};
	}
	ChunkWorkers()
	{
		try
		{
			for (unsigned i = 0; i < MaxWorkers; ++i)
				m_threads[i] = std::thread([this, i] { Work(i); });
		}
		catch (...)
		{
			Stop();
			throw;
		}
	}
	~ChunkWorkers()
	{
		Stop();
	}
	ChunkWorkers(const ChunkWorkers &) = delete;
	ChunkWorkers &operator=(const ChunkWorkers &) = delete;

	template <class F>
	ChunkTiming Run(size_t draws, unsigned requested, F &&record, bool profile = true)
	{
		using Clock = std::chrono::steady_clock;
		const auto start = profile ? Clock::now() : Clock::time_point{};
		std::unique_lock lock(m_mutex);
		if (m_remaining)
			throw std::logic_error("Concurrent Latte chunk dispatch");
		m_draws = draws;
		m_timing = {};
		m_timing.count = Count(draws, requested);
		if (!m_timing.count)
			return m_timing;
		m_job = std::forward<F>(record);
		m_profile = profile;
		m_error = nullptr;
		m_remaining = m_timing.count;
		++m_epoch;
		m_ready.notify_all();
		m_done.wait(lock, [this] { return m_remaining == 0; });
		m_job = {};
		if (m_error)
			std::rethrow_exception(m_error);
		if (profile)
			m_timing.criticalPathNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
		return m_timing;
	}

  private:
	void Stop()
	{
		{
			std::lock_guard lock(m_mutex);
			m_stop = true;
			m_ready.notify_all();
		}
		for (auto &thread : m_threads)
			if (thread.joinable())
				thread.join();
	}
	void Work(unsigned worker)
	{
		using Clock = std::chrono::steady_clock;
		uint64_t observed = 0;
		std::unique_lock lock(m_mutex);
		while (true)
		{
			m_ready.wait(lock, [&] { return m_stop || m_epoch != observed; });
			if (m_stop)
				return;
			observed = m_epoch;
			if (worker >= m_timing.count)
				continue;
			const auto range = Range(m_draws, m_timing.count, worker);
			const bool profile = m_profile;
			lock.unlock();
			const auto start = profile ? Clock::now() : Clock::time_point{};
			std::exception_ptr error;
			try
			{
				m_job(worker, range);
			}
			catch (...)
			{
				error = std::current_exception();
			}
			const auto elapsed = profile ? std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count() : 0;
			lock.lock();
			m_timing.busyNs[worker] = elapsed;
			if (error && !m_error)
				m_error = error;
			if (--m_remaining == 0)
				m_done.notify_one();
		}
	}
	std::array<std::thread, MaxWorkers> m_threads;
	std::mutex m_mutex;
	std::condition_variable m_ready, m_done;
	std::function<void(unsigned, ChunkRange)> m_job;
	std::exception_ptr m_error;
	ChunkTiming m_timing;
	size_t m_draws = 0;
	uint64_t m_epoch = 0;
	unsigned m_remaining = 0;
	bool m_stop = false, m_profile = false;
};
} // namespace LattePipeline
