#include "../src/serial_video_sender.h"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

int main()
{
	using namespace std::chrono_literals;
	drcd::SerialVideoSender sender;
	std::promise<void> entered, release, submitting;
	auto release_future = release.get_future().share();
	std::vector<int> order;
	if (!sender.Submit([&] { entered.set_value(); release_future.wait(); order.push_back(1); })) return 1;
	entered.get_future().wait();
	// Submit has returned while transmission is still blocked: the producer
	// can encode the next frame concurrently, without waiting for chunk sleeps.
	auto next = std::async(std::launch::async, [&] {
		submitting.set_value();
		return sender.Submit([&] { order.push_back(2); });
	});
	submitting.get_future().wait();
	const bool bounded = next.wait_for(20ms) == std::future_status::timeout;
	release.set_value();
	if (!next.get() || !sender.Finish() || !bounded || order != std::vector<int>{1,2}) return 1;
	if (sender.Submit([] {})) return 1;
	drcd::SerialVideoSender failing;
	if (!failing.Submit([] { throw std::runtime_error("synthetic send failure"); })) return 1;
	if (failing.Submit([] {}) || failing.Finish()) return 1;
	std::cout << "sender overlap, bounded handoff, FIFO, drain and failure tests passed\n";
}
