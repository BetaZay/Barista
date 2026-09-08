#include "api/legacy_control.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string_view>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

namespace barista::api
{
namespace
{
std::string JoinArgs(const std::vector<std::string>& args, size_t start_index)
{
	std::string out;
	for (size_t i = start_index; i < args.size(); ++i)
	{
		if (!out.empty())
			out.push_back(' ');
		out += args[i];
	}
	return out;
}

std::string BuildErrnoMessage(const std::string& prefix)
{
	return prefix + ": " + std::strerror(errno);
}
}

ParsedArgs parse_args(const std::vector<std::string>& args)
{
	ParsedArgs parsed;
	if (args.empty())
	{
		parsed.error = "no arguments";
		return parsed;
	}

	size_t command_index = 1;
	while (command_index < args.size())
	{
		if (args[command_index] == "--socket" || args[command_index] == "-s")
		{
			if ((command_index + 1) >= args.size())
			{
				parsed.error = "missing value for --socket";
				return parsed;
			}
			parsed.socket_path = args[command_index + 1];
			command_index += 2;
			continue;
		}
		if (args[command_index] == "--help" || args[command_index] == "-h")
		{
			parsed.ok = true;
			parsed.show_help = true;
			return parsed;
		}
		break;
	}

	if (command_index >= args.size())
	{
		parsed.error = "missing command";
		return parsed;
	}

	parsed.request = JoinArgs(args, command_index);
	if (parsed.request.empty())
	{
		parsed.error = "empty command";
		return parsed;
	}

	parsed.ok = true;
	return parsed;
}

bool send_request(const std::string& socket_path, const std::string& request, std::string& response, std::string& error)
{
	const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
	{
		error = BuildErrnoMessage("failed to create socket");
		return false;
	}

	timeval recv_timeout{};
	recv_timeout.tv_sec = 5;
	recv_timeout.tv_usec = 0;
	(void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

	if (socket_path.size() >= sizeof(sockaddr_un{}.sun_path))
	{
		error = "socket path too long";
		::close(fd);
		return false;
	}

	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path.c_str());
	if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
	{
		error = BuildErrnoMessage("failed to connect to drcd socket '" + socket_path + "'");
		::close(fd);
		return false;
	}

	const std::string payload = request + "\n";
	size_t offset = 0;
	while (offset < payload.size())
	{
		const auto n = ::write(fd, payload.data() + offset, payload.size() - offset);
		if (n <= 0)
		{
			error = BuildErrnoMessage("failed to write request");
			::close(fd);
			return false;
		}
		offset += static_cast<size_t>(n);
	}

	char buffer[512]{};
	while (true)
	{
		const auto n = ::read(fd, buffer, sizeof(buffer));
		if (n == 0)
			break;
		if (n < 0)
		{
			if ((errno == EAGAIN || errno == EWOULDBLOCK) && !response.empty())
				break;
			error = BuildErrnoMessage("timed out or failed while reading response");
			::close(fd);
			return false;
		}
		if (n > 0)
			response.append(buffer, static_cast<size_t>(n));
		else
			break;
	}

	::close(fd);
	return true;
}
}
