#include "drcd/interrupt.h"
#include "drcd/server_core.h"
#include "drc_host/version.h"
#include "drc_host/mac_address.h"
#include "drc_host/pairing.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include <sys/stat.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace
{
constexpr std::string_view kDefaultSocketPath = "/tmp/drcd.sock";

std::optional<std::string> ReadSavedApMac()
{
	const char* configured_path = std::getenv("DRCD_CREDENTIALS_FILE");
	const std::string path = configured_path && configured_path[0] ? configured_path : "/var/lib/drcd/credentials.conf";
	std::ifstream input(path);
	std::string line;
	while (std::getline(input, line))
	{
		if (line.rfind("ap_mac=", 0) == 0)
			return line.substr(7);
	}
	return std::nullopt;
}

std::optional<std::string> ReadInterfaceMac(const std::string& interface_name)
{
	std::ifstream input("/sys/class/net/" + interface_name + "/address");
	std::string mac;
	if (std::getline(input, mac) && !mac.empty())
		return mac;
	return std::nullopt;
}

void SignalHandler(int)
{
	drcd::set_stop_requested(true);
}

std::string ReadRequestLine(int client_fd)
{
	std::string request;
	char buffer[256]{};
	while (true)
	{
		const auto n = ::read(client_fd, buffer, sizeof(buffer));
		if (n <= 0)
			break;
		request.append(buffer, static_cast<size_t>(n));
		if (request.find('\n') != std::string::npos)
			break;
		if (request.size() > 4096)
			break;
	}

	const size_t newline = request.find('\n');
	if (newline != std::string::npos)
		request.resize(newline);
	return request;
}

bool WriteAll(int fd, const std::string& data)
{
	size_t offset = 0;
	while (offset < data.size())
	{
		// The status client can time out while an AP is starting.  A reply to
		// that closed socket must fail normally, not terminate the radio engine.
		const auto n = ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return false;
		offset += static_cast<size_t>(n);
	}
	return true;
}

std::optional<unsigned long> ParseUnsignedLongEnv(const char* name)
{
	const char* value = std::getenv(name);
	if (value == nullptr || value[0] == '\0')
		return std::nullopt;

	errno = 0;
	char* end_ptr = nullptr;
	const unsigned long parsed = std::strtoul(value, &end_ptr, 10);
	if (errno != 0 || end_ptr == value || *end_ptr != '\0')
		return std::nullopt;
	return parsed;
}

void ConfigureSocketOwnershipAndMode(const std::string& socket_path)
{
	uid_t owner_uid = ::geteuid();
	gid_t owner_gid = ::getegid();

	// If drcd is launched via sudo, hand the socket back to the invoking user.
	if (::geteuid() == 0)
	{
		const auto sudo_uid = ParseUnsignedLongEnv("SUDO_UID");
		const auto sudo_gid = ParseUnsignedLongEnv("SUDO_GID");
		if (sudo_uid.has_value() && sudo_gid.has_value() &&
			sudo_uid.value() <= std::numeric_limits<uid_t>::max() &&
			sudo_gid.value() <= std::numeric_limits<gid_t>::max())
		{
			owner_uid = static_cast<uid_t>(sudo_uid.value());
			owner_gid = static_cast<gid_t>(sudo_gid.value());
		}
	}

	if (::chown(socket_path.c_str(), owner_uid, owner_gid) != 0)
	{
		std::cerr << "drcd: warning: chown(" << socket_path << ") failed: " << std::strerror(errno) << "\n";
	}

	// Group-writable socket enables local client access after sudo launch.
	if (::chmod(socket_path.c_str(), static_cast<mode_t>(0660)) != 0)
	{
		std::cerr << "drcd: warning: chmod(" << socket_path << ") failed: " << std::strerror(errno) << "\n";
	}
}
}

int main(int argc, char** argv)
{
	drcd::set_stop_requested(false);

	std::string socket_path = std::string{kDefaultSocketPath};
	std::string interface_name = "wlan0";
	std::string ap_mac_text;
	int pair_code_value = 2220;
	bool automatic = true;
	bool pairing_enabled = true;
	bool start_in_pairing = false;
	std::string test_media_path;
	bool test_black_frames = false;
	for (int i = 1; i < argc; ++i)
	{
		const std::string_view arg = argv[i];
		if ((arg == "--socket" || arg == "-s") && (i + 1) < argc)
		{
			socket_path = argv[++i];
			continue;
		}
		if (arg == "--interface" && (i + 1) < argc)
		{
			interface_name = argv[++i];
			continue;
		}
		if (arg == "--ap-mac" && (i + 1) < argc)
		{
			ap_mac_text = argv[++i];
			continue;
		}
		if (arg == "--pair-code" && (i + 1) < argc)
		{
			try { pair_code_value = std::stoi(argv[++i]); }
			catch (...) { std::cerr << "drcd: invalid pairing code\n"; return 1; }
			continue;
		}
		if (arg == "--manual")
		{
			automatic = false;
			continue;
		}
		if (arg == "--np")
		{
			pairing_enabled = false;
			continue;
		}
		if (arg == "--pair")
		{
			start_in_pairing = true;
			continue;
		}
		if (arg == "--play" && (i + 1) < argc)
		{
			test_media_path = argv[++i];
			continue;
		}
		if (arg == "--black")
		{
			test_black_frames = true;
			continue;
		}
		if (arg == "--help" || arg == "-h")
		{
			std::cout
				<< "drcd " << drc_host::version() << "\n"
				<< "Usage: drcd [--socket <path>] [--interface <iface>] [--ap-mac <mac>]\n"
				<< "            [--pair-code <digits 0-3>] [--play <media> | --black] [--np] [--manual]\n"
				<< "  --np            Check for paired GamePads without entering pairing mode\n"
				<< "  --pair          Enter pairing mode immediately\n"
				<< "  --play <media>  Loop audio/video to the GamePad after it connects\n"
				<< "  --black         Stream built-in black video frames without audio\n";
			return 0;
		}
	}
	if (test_black_frames && !test_media_path.empty())
	{
		std::cerr << "drcd: --play and --black are mutually exclusive\n";
		return 1;
	}

	if (socket_path.empty())
	{
		std::cerr << "drcd: socket path is empty\n";
		return 1;
	}
	if (socket_path.size() >= sizeof(sockaddr_un{}.sun_path))
	{
		std::cerr << "drcd: socket path too long\n";
		return 1;
	}

	std::signal(SIGINT, SignalHandler);
	std::signal(SIGTERM, SignalHandler);

	const int server_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (server_fd < 0)
	{
		std::perror("drcd socket");
		return 1;
	}

	::unlink(socket_path.c_str());
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path.c_str());

	if (::bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
	{
		std::perror("drcd bind");
		::close(server_fd);
		return 1;
	}
	ConfigureSocketOwnershipAndMode(socket_path);

	if (::listen(server_fd, 16) != 0)
	{
		std::perror("drcd listen");
		::close(server_fd);
		::unlink(socket_path.c_str());
		return 1;
	}

	std::cout << "drcd " << drc_host::version() << " listening on " << socket_path << "\n";

	drcd::ServerCore core;
	if (automatic)
	{
		if (ap_mac_text.empty())
		{
			if (const char* env_mac = std::getenv("DRCD_AP_MAC"); env_mac && env_mac[0])
				ap_mac_text = env_mac;
			else if (const auto saved_mac = ReadSavedApMac(); saved_mac.has_value())
				ap_mac_text = *saved_mac;
			else if (const auto interface_mac = ReadInterfaceMac(interface_name); interface_mac.has_value())
				ap_mac_text = *interface_mac;
		}
		const auto ap_mac = drc_host::MacAddress::parse(ap_mac_text);
		const auto pair_code = drc_host::PairingCode::from_numeric(static_cast<uint16_t>(pair_code_value));
		if (!ap_mac.has_value() || !pair_code.has_value())
		{
			std::cerr << "drcd: automatic mode needs a valid AP MAC and pairing code\n";
			::close(server_fd);
			::unlink(socket_path.c_str());
			return 1;
		}
		core.start_automatic({
			.request = {
				.interface_name = interface_name,
				.ap_mac = *ap_mac,
				.pairing_code = *pair_code,
				.test_media_path = test_media_path,
				.test_black_frames = test_black_frames,
			},
			.pairing_enabled = pairing_enabled,
			.start_in_pairing = start_in_pairing,
		});
	}
	while (!drcd::is_stop_requested())
	{
		pollfd listener{};
		listener.fd = server_fd;
		listener.events = POLLIN;
		const int poll_result = ::poll(&listener, 1, 250);
		if (poll_result < 0)
		{
			if (errno == EINTR)
				continue;
			std::perror("drcd poll");
			break;
		}
		core.process_backend_events();
		if (poll_result == 0)
			continue;
		if ((listener.revents & POLLIN) == 0)
		{
			if ((listener.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
				break;
			continue;
		}

		const int client_fd = ::accept(server_fd, nullptr, nullptr);
		if (client_fd < 0)
		{
			if (drcd::is_stop_requested())
				break;
			continue;
		}

		const std::string request_line = ReadRequestLine(client_fd);
		const auto response = core.handle_line(request_line);
		WriteAll(client_fd, response.payload);
		(void)::shutdown(client_fd, SHUT_WR);
		::close(client_fd);

		if (response.shutdown_requested)
			break;
	}

	::close(server_fd);
	::unlink(socket_path.c_str());
	return 0;
}
