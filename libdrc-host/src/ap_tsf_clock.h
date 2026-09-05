#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace drc_host
{
// RTL8852BE AX bank 0, port 0. Accesses only the debug register-read selector.
// No hardware writes. Do not use another debug register reader concurrently.
inline std::optional<uint32_t> ReadApTsfRegister(const std::string& path, uint32_t address)
{
	FILE* file = std::fopen(path.c_str(), "w");
	if (!file)
		return {};
	const bool selected = std::fprintf(file, "%x 4\n", address) > 0;
	const bool closed = std::fclose(file) == 0;
	if (!selected || !closed)
		return {};
	file = std::fopen(path.c_str(), "r");
	if (!file)
		return {};
	unsigned int bytes = 0, actual = 0, value = 0;
	const int fields = std::fscanf(file, "get %u bytes at 0x%x=0x%x", &bytes, &actual, &value);
	std::fclose(file);
	if (fields != 3 || bytes != 4 || actual != address || value == 0xdeadbeef || value == 0xffffffff)
		return {};
	return value;
}

inline std::optional<uint64_t> ReadApPortTsf(const std::string& path)
{
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		const auto high1 = ReadApTsfRegister(path, 0xc43c);
		const auto low = ReadApTsfRegister(path, 0xc438);
		const auto high2 = ReadApTsfRegister(path, 0xc43c);
		if (!high1 || !low || !high2)
			return {};
		if (*high1 != *high2)
			continue;
		const uint64_t tsf = (uint64_t{*high1} << 32) | *low;
		return tsf > 1 ? std::optional<uint64_t>{tsf} : std::nullopt;
	}
	return {};
}

class ApTsfClock
{
public:
	using Clock = std::chrono::steady_clock;
	using Reader = std::function<std::optional<uint64_t>()>;
	explicit ApTsfClock(Reader reader) : m_reader(std::move(reader)) {}

	bool update()
	{
		const auto before = Clock::now();
		const auto value = m_reader();
		const auto after = Clock::now();
		const auto midpoint = before + (after - before) / 2;
		std::lock_guard lock(m_mutex);
		if (!value || *value <= 1 || after - before > std::chrono::milliseconds(5))
		{
			m_healthy = false;
			return false;
		}
		if (m_initialized)
		{
			const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(midpoint - m_sampled_at).count();
			if (*value <= m_tsf)
			{
				m_healthy = false;
				return false;
			}
			const int64_t difference = static_cast<int64_t>(*value - m_tsf) - elapsed;
			if (*value <= m_tsf || difference < -20000 || difference > 20000)
			{
				m_healthy = false;
				return false;
			}
		}
		m_tsf = *value;
		m_sampled_at = midpoint;
		m_initialized = m_healthy = true;
		return true;
	}

	uint32_t timestamp() const
	{
		std::lock_guard lock(m_mutex);
		const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - m_sampled_at).count();
		return static_cast<uint32_t>(m_tsf + elapsed);
	}

	bool healthy() const
	{
		std::lock_guard lock(m_mutex);
		return m_healthy && Clock::now() - m_sampled_at < std::chrono::seconds(1);
	}

private:
	Reader m_reader;
	mutable std::mutex m_mutex;
	uint64_t m_tsf = 0;
	Clock::time_point m_sampled_at{};
	bool m_initialized = false;
	bool m_healthy = false;
};
}
