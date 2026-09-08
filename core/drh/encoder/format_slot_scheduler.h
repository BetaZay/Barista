#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace barista::drh
{
// The format clock must continue while the encoder is busy or recovery omits
// video. One producer reserves one future slot; encoded reference frames are
// never dropped. The callback publishes the format and returns its timestamp.
class FormatSlotScheduler
{
public:
	using Clock = std::chrono::steady_clock;
	struct Slot { uint32_t timestamp{}; Clock::time_point published{}; };
	using Publish = std::function<uint32_t(std::optional<uint32_t>)>;
	explicit FormatSlotScheduler(Publish publish) : m_publish(std::move(publish)),
		m_thread([this] { Run(); }) {}
	~FormatSlotScheduler() { Finish(); }
	FormatSlotScheduler(const FormatSlotScheduler&) = delete;
	FormatSlotScheduler& operator=(const FormatSlotScheduler&) = delete;

	bool Reserve(bool idr, std::optional<uint32_t> legacy_timestamp, Slot& slot)
	{
		std::unique_lock lock(m_mutex);
		if (m_closing) return false;
		m_pending = true;
		m_idr = idr;
		m_legacy = legacy_timestamp;
		m_changed.wait(lock, [this] { return !m_pending || m_closing; });
		if (m_closing) return false;
		slot = m_slot;
		return true;
	}

	bool Finish()
	{
		{
			std::lock_guard lock(m_mutex);
			m_closing = true;
		}
		m_changed.notify_all();
		if (m_thread.joinable()) m_thread.join();
		return !m_failed;
	}

private:
	void Run()
	{
		std::unique_lock lock(m_mutex);
		auto deadline = Clock::now();
		// 1001/60000 seconds, retaining fractional microseconds on the host.
		constexpr auto period = std::chrono::nanoseconds(16683333);
		while (!m_closing)
		{
			if (m_changed.wait_until(lock, deadline, [this] { return m_closing; })) break;
			const bool video = m_pending && !m_gap;
			try
			{
				const auto timestamp = m_publish(video ? m_legacy : std::nullopt);
				if (video) m_slot = {timestamp, Clock::now()};
			}
			catch (...)
			{
				m_failed = m_closing = true;
				m_changed.notify_all();
				break;
			}
			// Every IDR is followed by a format-only slot, including startup.
			m_gap = video && m_idr;
			if (video) { m_pending = false; m_changed.notify_all(); }
			deadline += period;
			// Never burst stale formats after a scheduler or transport stall.
			if (deadline <= Clock::now()) deadline = Clock::now() + period;
		}
	}
	Publish m_publish;
	std::mutex m_mutex;
	std::condition_variable m_changed;
	bool m_closing = false, m_failed = false, m_pending = false, m_idr = false, m_gap = false;
	std::optional<uint32_t> m_legacy;
	Slot m_slot;
	std::thread m_thread;
};
}
