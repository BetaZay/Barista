#include "drh/ap_tsf_clock.h"

#include <cstdlib>
#include <iostream>
#include <thread>

namespace
{
void expect(bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << message << '\n';
		std::exit(1);
	}
}
}

int main()
{
	using namespace std::chrono;
	const auto origin = steady_clock::now();
	bool unavailable = false;
	bool reset = false;
	barista::drh::ApTsfClock clock([&]() -> std::optional<uint64_t> {
		if (unavailable) return {};
		if (reset) return 2;
		return 1000000 + duration_cast<microseconds>(steady_clock::now() - origin).count();
	});
	expect(!clock.healthy(), "uninitialized clock must not enable media");
	expect(clock.update() && clock.healthy(), "valid first sample rejected");
	const auto first = clock.timestamp();
	std::this_thread::sleep_for(milliseconds(3));
	expect(clock.timestamp() > first + 1000, "clock did not extrapolate between samples");
	expect(clock.update(), "advancing register sample rejected");
	unavailable = true;
	expect(!clock.update() && !clock.healthy(), "failed read must disable media");
	unavailable = false;
	std::this_thread::sleep_for(milliseconds(2));
	expect(clock.update() && clock.healthy(), "transient read failure did not recover");
	reset = true;
	expect(!clock.update() && !clock.healthy(), "clock reset must not silently change timestamp epoch");

	barista::drh::ApTsfClock stopped([] { return std::optional<uint64_t>{1}; });
	expect(!stopped.update(), "inactive port value accepted");
	barista::drh::ApTsfClock frozen([] { return std::optional<uint64_t>{1000}; });
	expect(frozen.update() && !frozen.update(), "nonadvancing port accepted");

	const auto wrap_origin = steady_clock::now();
	barista::drh::ApTsfClock wrap([&]() -> std::optional<uint64_t> {
		return 0xfffffff0ULL + duration_cast<microseconds>(steady_clock::now() - wrap_origin).count();
	});
	expect(wrap.update(), "wrap clock initialization failed");
	std::this_thread::sleep_for(milliseconds(2));
	expect(wrap.timestamp() < 1000000, "32-bit wire timestamp failed to wrap");
	expect(wrap.update(), "64-bit TSF sample failed across 32-bit wire wrap");
	std::this_thread::sleep_for(milliseconds(1010));
	expect(!wrap.healthy(), "stale clock must disable media");
	std::cout << "AP TSF clock tests passed\n";
}
