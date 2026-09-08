#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace barista::drh
{
// One in-flight send, no pending queue. The producer may encode one next
// frame concurrently, but cannot submit it until the preceding send finishes.
class SerialVideoSender
{
public:
	SerialVideoSender() : m_thread([this] { Run(); }) {}
	~SerialVideoSender() { Finish(); }
	SerialVideoSender(const SerialVideoSender&) = delete;
	SerialVideoSender& operator=(const SerialVideoSender&) = delete;

	bool Submit(std::function<void()> task)
	{
		std::unique_lock lock(m_mutex);
		m_changed.wait(lock, [this] { return !m_busy; });
		if (m_failed || m_closing) return false;
		m_task = std::move(task);
		m_busy = true;
		m_changed.notify_all();
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
		for (;;)
		{
			m_changed.wait(lock, [this] { return m_closing || bool(m_task); });
			if (!m_task) return;
			auto task = std::move(m_task);
			m_task = {};
			lock.unlock();
			bool failed = false;
			try { task(); } catch (...) { failed = true; }
			lock.lock();
			m_failed |= failed;
			m_busy = false;
			m_changed.notify_all();
		}
	}

	std::mutex m_mutex;
	std::condition_variable m_changed;
	std::function<void()> m_task;
	bool m_busy = false;
	bool m_closing = false;
	bool m_failed = false;
	std::thread m_thread;
};
}
