#pragma once

#include "Cafe/HW/Latte/Core/LatteIndexConversion.h"
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <vector>

namespace LattePipeline
{
// Guest bytes are copied at decode time. The CPU range decision is serial;
// endian conversion and the output write run once on the first chunk that uses
// the allocation. Sibling chunks may share it without racing on upload memory.
class DeferredIndex
{
  public:
	static uint32_t Maximum(std::span<const std::byte> bytes, uint32_t width, bool bigEndian)
	{
		if ((width != 2 && width != 4) || bytes.size() % width)
			throw std::invalid_argument("Invalid Latte deferred index shape");
		uint32_t maximum = 0;
		for (size_t i = 0; i < bytes.size(); i += width)
		{
			uint32_t value = 0;
			for (uint32_t j = 0; j < width; ++j)
				value |= uint32_t(std::to_integer<uint8_t>(bytes[i + j])) << ((bigEndian ? width - 1 - j : j) * 8);
			maximum = std::max(maximum, value);
		}
		return maximum;
	}
	DeferredIndex(std::span<const std::byte> bytes, uint32_t width, bool bigEndian, void *destination)
		: m_source(bytes.begin(), bytes.end()), m_width(width), m_bigEndian(bigEndian), m_destination(destination)
	{
		if (!destination || (width != 2 && width != 4) || bytes.size() % width)
			throw std::invalid_argument("Invalid Latte deferred index destination");
	}
	void Finish()
	{
		std::call_once(m_once, [&] {
			uint32_t maximum = 0;
			const auto count = static_cast<uint32_t>(m_source.size() / m_width);
			if (!m_bigEndian)
				std::memcpy(m_destination, m_source.data(), m_source.size());
			else if (m_width == 2)
				LatteIndexConversion::CopyBigEndian<uint16_t>(m_source.data(), m_destination, count, maximum);
			else
				LatteIndexConversion::CopyBigEndian<uint32_t>(m_source.data(), m_destination, count, maximum);
			// No worker reads the input after this once edge. Release the extra
			// raw copy while preserving the bounded front cache's exact shadow.
			std::vector<std::byte>{}.swap(m_source);
		});
	}

  private:
	std::once_flag m_once;
	std::vector<std::byte> m_source;
	uint32_t m_width;
	bool m_bigEndian;
	void *m_destination;
};
} // namespace LattePipeline
