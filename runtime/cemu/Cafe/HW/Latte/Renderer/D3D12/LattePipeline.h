#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

// Platform-neutral handoff contracts, also exercised by the host tests.
namespace LattePipeline
{
inline bool EnvironmentFlag(const char *name)
{
	const char *value = std::getenv(name);
	return value && std::strcmp(value, "1") == 0;
}

class Context
{
  public:
	Context() : m_front(std::this_thread::get_id()), m_back(m_front)
	{
	}
	bool Queued() const
	{
		return m_queued;
	}
	void CheckFront() const
	{
		Check(m_front, "front");
	}
	void CheckBack() const
	{
		Check(m_back, "back");
	}
	uint64_t BeginOperation()
	{
		CheckFront();
		return ++m_sequence;
	}
	uint64_t Sequence() const
	{
		return m_sequence;
	}
	uint64_t Frame() const
	{
		return m_frame;
	}
	void EndFrame()
	{
		CheckFront();
		++m_frame;
	}

  private:
	void Check(std::thread::id owner, const char *role) const
	{
#if defined(NDEBUG) && !defined(_DEBUG)
		if (!m_checks)
			return;
#endif
		if (owner != std::this_thread::get_id())
		{
			std::fprintf(stderr, "Latte pipeline: %s ownership violation at operation %llu, frame %llu\n", role,
						 static_cast<unsigned long long>(m_sequence), static_cast<unsigned long long>(m_frame));
			std::abort();
		}
	}
	const std::thread::id m_front, m_back;
	const bool m_checks = EnvironmentFlag("CEMU_LATTE_PIPELINE_CHECKS");
	const bool m_queued = EnvironmentFlag("CEMU_LATTE_PIPELINE");
	uint64_t m_sequence = 0;
	uint64_t m_frame = 1;
};
} // namespace LattePipeline
