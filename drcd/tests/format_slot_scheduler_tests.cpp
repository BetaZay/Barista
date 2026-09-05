#include "../src/format_slot_scheduler.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <vector>

int main()
{
	using namespace std::chrono;
	using Scheduler = drcd::FormatSlotScheduler;
	std::atomic_uint published{0};
	std::vector<Scheduler::Clock::time_point> times;
	Scheduler scheduler([&](std::optional<uint32_t> legacy) {
		times.push_back(Scheduler::Clock::now());
		const auto stamp = ++published;
		return legacy.value_or(stamp);
	});
	Scheduler::Slot idr, predicted, ordinary;
	if (!scheduler.Reserve(true, {}, idr) || !scheduler.Reserve(false, {}, predicted)) return 1;
	if (predicted.timestamp != idr.timestamp + 2) return 1;
	if (!scheduler.Reserve(false, {}, ordinary) || ordinary.timestamp != predicted.timestamp + 1) return 1;
	// Simulate slow encoding without reserving video: the format clock continues.
	const auto before = published.load();
	std::this_thread::sleep_for(milliseconds(85));
	if (published.load() < before + 4) return 1;
	Scheduler::Slot legacy;
	if (!scheduler.Reserve(false, 12345, legacy) || legacy.timestamp != 12345) return 1;
	if (!scheduler.Finish() || scheduler.Reserve(false, {}, ordinary)) return 1;
	for (size_t i = 1; i < times.size(); ++i)
		if (times[i] - times[i - 1] < milliseconds(8)) return 1;
	// A blocked publisher must not cause a catch-up burst of stale formats.
	std::vector<Scheduler::Clock::time_point> stalled_times;
	Scheduler stalled([&](std::optional<uint32_t>) {
		stalled_times.push_back(Scheduler::Clock::now());
		if (stalled_times.size() == 1) std::this_thread::sleep_for(milliseconds(45));
		return uint32_t(stalled_times.size());
	});
	std::this_thread::sleep_for(milliseconds(110));
	if (!stalled.Finish() || stalled_times.size() < 3) return 1;
	for (size_t i = 1; i < stalled_times.size(); ++i)
		if (stalled_times[i] - stalled_times[i - 1] < milliseconds(8)) return 1;
	Scheduler failed([](std::optional<uint32_t>) -> uint32_t { throw std::runtime_error("send failed"); });
	if (failed.Reserve(false, {}, ordinary) || failed.Finish()) return 1;
	std::cout << "Independent format clock, IDR gap, reference ordering, legacy stamp, stall and failure handling passed\n";
}
