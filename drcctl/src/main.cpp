#include "drcctl/client.h"
#include "drc_host/version.h"

#include <iostream>
#include <vector>

namespace
{
void PrintUsage()
{
	std::cout
		<< "drcctl " << drc_host::version() << "\n"
		<< "Usage:\n"
		<< "  drcctl [--socket <path>] status\n"
		<< "  drcctl [--socket <path>] pair-start <iface> <ap-mac> <code>\n"
		<< "  drcctl [--socket <path>] pair-complete\n"
		<< "  drcctl [--socket <path>] pair-stop\n"
		<< "  drcctl [--socket <path>] runtime-start\n"
		<< "  drcctl [--socket <path>] runtime-stop\n"
		<< "  drcctl [--socket <path>] set-connected <0|1>\n"
		<< "  drcctl [--socket <path>] shutdown\n";
}
}

int main(int argc, char** argv)
{
	std::vector<std::string> args;
	args.reserve(static_cast<size_t>(argc));
	for (int i = 0; i < argc; ++i)
		args.emplace_back(argv[i]);

	const auto parsed = drcctl::parse_args(args);
	if (!parsed.ok)
	{
		if (!parsed.error.empty())
			std::cerr << "drcctl: " << parsed.error << "\n";
		PrintUsage();
		return 1;
	}
	if (parsed.show_help)
	{
		PrintUsage();
		return 0;
	}

	std::string response;
	std::string error;
	if (!drcctl::send_request(parsed.socket_path, parsed.request, response, error))
	{
		std::cerr << "drcctl: " << error << "\n";
		return 1;
	}

	std::cout << response;
	return response.rfind("ERR ", 0) == 0 ? 2 : 0;
}
