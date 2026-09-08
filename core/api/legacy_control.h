#pragma once

#include <string>
#include <vector>

namespace barista::api
{
struct ParsedArgs
{
	bool ok = false;
	bool show_help = false;
	std::string error;
	std::string socket_path = "/tmp/drcd.sock";
	std::string request;
};

ParsedArgs parse_args(const std::vector<std::string>& args);
bool send_request(const std::string& socket_path, const std::string& request, std::string& response, std::string& error);
}
