#include "api/legacy_control.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
void expect(bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "test failure: " << message << '\n';
		std::exit(1);
	}
}
}

int main()
{
	{
		const std::vector<std::string> args{"drcctl", "status"};
		const auto parsed = barista::api::parse_args(args);
		expect(parsed.ok, "status parse should succeed");
		expect(!parsed.show_help, "status parse should not show help");
		expect(parsed.request == "status", "status request mismatch");
		expect(parsed.socket_path == "/tmp/drcd.sock", "default socket path mismatch");
	}

	{
		const std::vector<std::string> args{"drcctl", "--socket", "/tmp/custom.sock", "pair-start", "wlan0", "f8:54:f6:7a:5c:ae", "2232"};
		const auto parsed = barista::api::parse_args(args);
		expect(parsed.ok, "pair-start parse should succeed");
		expect(parsed.request == "pair-start wlan0 f8:54:f6:7a:5c:ae 2232", "pair-start request mismatch");
		expect(parsed.socket_path == "/tmp/custom.sock", "custom socket path mismatch");
	}

	{
		const std::vector<std::string> args{"drcctl", "--help"};
		const auto parsed = barista::api::parse_args(args);
		expect(parsed.ok, "help parse should succeed");
		expect(parsed.show_help, "help parse should set show_help");
	}

	{
		const std::vector<std::string> args{"drcctl", "--socket"};
		const auto parsed = barista::api::parse_args(args);
		expect(!parsed.ok, "incomplete socket option should fail");
	}

	std::cout << "drcctl_core_tests: ok\n";
	return 0;
}
