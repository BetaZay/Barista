#include "drc_host/runtime_transport.h"
#include "ap_tsf_clock.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace drc_host
{
namespace
{
constexpr uint16_t kConsoleMessagePort = 50010;
constexpr uint16_t kConsoleVideoPort = 50020;
constexpr uint16_t kConsoleAudioPort = 50021;
constexpr uint16_t kConsoleInputPort = 50022;
constexpr uint16_t kConsoleCommandPort = 50023;
constexpr uint16_t kConsoleUnknownPort = 50025;
constexpr uint16_t kGamepadVideoPort = 50120;
constexpr uint16_t kGamepadAudioPort = 50121;
constexpr uint16_t kGamepadCommandPort = 50123;
constexpr uint16_t kGamepadUnknownPort = 50125;
constexpr size_t kCommandHeaderSize = 8;
constexpr int kDrcStickMin = 900;
constexpr int kDrcStickMax = 3200;
constexpr int kStickLogThreshold = 64;
constexpr auto kStickLogInterval = std::chrono::milliseconds(125);
constexpr auto kCommandTimeout = std::chrono::seconds(1);
constexpr int kCommandMaxAttempts = 10;
constexpr int kMediaSocketPriority = 5;
constexpr int kMediaIpTos = 0xa0;

struct InputSnapshot
{
	uint16_t sequence = 0;
	uint32_t buttons = 0;
	std::array<uint16_t, 4> sticks{};
	uint8_t power_status = 0;
	uint8_t battery_charge = 0;
	uint8_t audio_volume = 0;
	bool waiting_for_streaming = true;
};

uint16_t ReadLe16Unaligned(const uint8_t* data)
{
	return static_cast<uint16_t>(data[0]) |
		(static_cast<uint16_t>(data[1]) << 8);
}

uint32_t ReadLe32Unaligned(const uint8_t* data)
{
	return static_cast<uint32_t>(data[0]) |
		(static_cast<uint32_t>(data[1]) << 8) |
		(static_cast<uint32_t>(data[2]) << 16) |
		(static_cast<uint32_t>(data[3]) << 24);
}

uint64_t ReadLe64Unaligned(const uint8_t* data)
{
	uint64_t value = 0;
	for (size_t i = 0; i < sizeof(value); ++i)
		value |= static_cast<uint64_t>(data[i]) << (i * 8);
	return value;
}

std::optional<uint64_t> ReadRadiotapTsf(std::span<const uint8_t> packet)
{
	// Radiotap version, pad, length, then one or more presence words. TSFT is
	// field zero and is aligned to an eight-byte boundary from the start of the
	// radiotap header.
	if (packet.size() < 8 || packet[0] != 0)
		return std::nullopt;
	const size_t radiotap_length = ReadLe16Unaligned(packet.data() + 2);
	if (radiotap_length < 8 || radiotap_length > packet.size())
		return std::nullopt;

	const uint32_t first_presence = ReadLe32Unaligned(packet.data() + 4);
	if ((first_presence & 1u) == 0)
		return std::nullopt;

	size_t offset = 4;
	uint32_t presence = 0;
	do
	{
		if (offset + 4 > radiotap_length)
			return std::nullopt;
		presence = ReadLe32Unaligned(packet.data() + offset);
		offset += 4;
	} while ((presence & 0x80000000u) != 0);

	offset = (offset + 7u) & ~size_t{7u};
	if (offset + sizeof(uint64_t) > radiotap_length)
		return std::nullopt;
	return ReadLe64Unaligned(packet.data() + offset);
}

int64_t SteadyMicroseconds()
{
	using namespace std::chrono;
	return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

uint16_t ReadLe16(const uint8_t* data)
{
	return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint16_t ReadBe16(const uint8_t* data)
{
	return static_cast<uint16_t>(static_cast<uint16_t>(data[0]) << 8) |
		static_cast<uint16_t>(data[1]);
}

InputSnapshot DecodeInput(std::span<const uint8_t> packet)
{
	InputSnapshot input;
	input.sequence = ReadBe16(packet.data());
	input.buttons = (static_cast<uint32_t>(packet[80]) << 16) |
		(static_cast<uint32_t>(packet[2]) << 8) | packet[3];
	for (size_t i = 0; i < input.sticks.size(); ++i)
		input.sticks[i] = ReadLe16(packet.data() + 6 + i * 2);
	input.power_status = packet[4];
	input.battery_charge = packet[5];
	input.audio_volume = packet[14];
	input.waiting_for_streaming = (packet[83] & 1u) != 0;
	return input;
}

std::string ButtonNames(uint32_t buttons)
{
	struct Button
	{
		uint32_t mask;
		const char* name;
	};
	static constexpr std::array<Button, 19> kButtons{{
		{0x00008000, "A"}, {0x00004000, "B"},
		{0x00002000, "X"}, {0x00001000, "Y"},
		{0x00000020, "L"}, {0x00000010, "R"},
		{0x00000080, "ZL"}, {0x00000040, "ZR"},
		{0x00000004, "MINUS"}, {0x00000008, "PLUS"},
		{0x00000002, "HOME"}, {0x00000800, "LEFT"},
		{0x00000400, "RIGHT"}, {0x00000100, "DOWN"},
		{0x00000200, "UP"}, {0x00000001, "SYNC"},
		{0x00800000, "L3"}, {0x00400000, "R3"},
		{0x00200000, "TV"},
	}};

	std::string names;
	for (const auto& button : kButtons)
	{
		if ((buttons & button.mask) == 0)
			continue;
		if (!names.empty())
			names += '+';
		names += button.name;
	}
	return names.empty() ? "none" : names;
}

float NormalizeStick(uint16_t raw)
{
	const int clamped = std::clamp<int>(raw, kDrcStickMin, kDrcStickMax);
	const int midpoint = (kDrcStickMax - kDrcStickMin) / 2;
	float value = static_cast<float>(clamped - (kDrcStickMin + midpoint)) /
		static_cast<float>(midpoint);
	if (value > -0.1f && value < 0.1f)
		value = 0.0f;
	return value;
}

std::string FormatInput(const InputSnapshot& input)
{
	std::ostringstream line;
	line << "Input: seq=" << input.sequence
		<< " buttons=" << ButtonNames(input.buttons)
		<< std::fixed << std::setprecision(2)
		<< " L=(" << NormalizeStick(input.sticks[0]) << ',' << NormalizeStick(input.sticks[1]) << ')'
		<< " R=(" << NormalizeStick(input.sticks[2]) << ',' << NormalizeStick(input.sticks[3]) << ')'
		<< " raw=" << input.sticks[0] << ',' << input.sticks[1] << ','
		<< input.sticks[2] << ',' << input.sticks[3]
		<< " battery=" << static_cast<unsigned int>(input.battery_charge)
		<< " volume=" << static_cast<unsigned int>(input.audio_volume);
	return line.str();
}

void WriteLe16(uint8_t* data, uint16_t value)
{
	data[0] = static_cast<uint8_t>(value);
	data[1] = static_cast<uint8_t>(value >> 8);
}

std::vector<uint8_t> BuildCommand(uint16_t type, uint16_t query_type, uint16_t sequence,
	std::span<const uint8_t> payload)
{
	std::vector<uint8_t> packet(kCommandHeaderSize + payload.size());
	WriteLe16(packet.data(), type);
	WriteLe16(packet.data() + 2, query_type);
	WriteLe16(packet.data() + 4, static_cast<uint16_t>(payload.size()));
	WriteLe16(packet.data() + 6, sequence);
	std::copy(payload.begin(), payload.end(), packet.begin() + kCommandHeaderSize);
	return packet;
}

std::vector<uint8_t> BuildUicConfigQuery()
{
	// Generic command: final fragment, query flag, peripheral service (5),
	// read EEPROM/config method (6).
	return {0x7e, 0x01, 0x00, 0x08, 0x00, 0x40, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00};
}

std::vector<uint8_t> BuildUvcUacQuery()
{
	std::vector<uint8_t> payload(48);
	// Authenticated real-console capture uses little endian for these fields.
	WriteLe16(payload.data() + 12, 16000);
	return payload;
}

std::string_view CommandPacketTypeName(uint16_t type)
{
	switch (type)
	{
	case 0: return "query";
	case 1: return "query-ack";
	case 2: return "reply";
	case 3: return "reply-ack";
	default: return "invalid";
	}
}

std::string_view CommandQueryTypeName(uint16_t type)
{
	switch (type)
	{
	case 0: return "UIC";
	case 1: return "UVC/UAC";
	case 2: return "time";
	default: return "invalid";
	}
}

bool ValidateCommandReply(uint16_t query_type, std::span<const uint8_t> payload,
	std::string& reason)
{
	if (query_type == 0)
	{
		if (payload.size() != 0x310)
		{
			reason = "UIC reply size " + std::to_string(payload.size()) + " (expected 784)";
			return false;
		}
		if (payload[0] != 0x7e || payload[1] != 0x01 || payload[6] != 0x05 ||
			payload[7] != 0x06 || payload[8] != 0 || payload[9] != 0)
		{
			reason = "UIC generic-command header mismatch";
			return false;
		}
		const size_t inner_size = (static_cast<size_t>(payload[10]) << 8) | payload[11];
		if (inner_size != payload.size() - 12)
		{
			reason = "UIC inner payload length mismatch";
			return false;
		}
	}
	else if (query_type == 1 && payload.size() != 16)
	{
		reason = "UVC/UAC reply size " + std::to_string(payload.size()) + " (expected 16)";
		return false;
	}
	return true;
}

void SynchronizeUvcUacRequest(std::vector<uint8_t>& request,
	std::span<const uint8_t> response)
{
	// Compatibility experiment: the real console changes byte 0 from 0 to 1
	// after its first exchange. Its precise meaning remains unconfirmed.
	request[0] = 1;
	// Both response and request volume fields are little endian in the
	// authenticated real-console capture (unlike the old libdrc request code).
	const uint16_t mic_volume = ReadLe16(response.data());
	const uint16_t mic_jack_volume = ReadLe16(response.data() + 2);
	request[4] = response[12] != 0;
	WriteLe16(request.data() + 6, mic_volume);
	WriteLe16(request.data() + 8, mic_jack_volume);
	request[16] = response[13];
	request[17] = response[14] != 0;
}

enum class CommandStage
{
	Idle,
	AwaitingAck,
	AwaitingReply,
};

struct CommandTransaction
{
	uint16_t query_type = 0;
	uint16_t sequence = 0;
	std::vector<uint8_t> payload;
	CommandStage stage = CommandStage::Idle;
	int attempts = 0;
	std::chrono::steady_clock::time_point first_sent{};
	std::chrono::steady_clock::time_point last_sent{};
	std::chrono::steady_clock::time_point next_due{};
};

int MakeSocket(const RuntimeTransportConfig& config, uint16_t port, bool media_socket,
	std::string& error)
{
	const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
	if (fd < 0)
	{
		error = std::strerror(errno);
		return -1;
	}
	const int one = 1;
	(void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (media_socket)
	{
		if (::setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &kMediaSocketPriority,
			sizeof(kMediaSocketPriority)) != 0)
		{
			error = "media SO_PRIORITY: " + std::string(std::strerror(errno));
			::close(fd);
			return -1;
		}
		if (::setsockopt(fd, IPPROTO_IP, IP_TOS, &kMediaIpTos, sizeof(kMediaIpTos)) != 0)
		{
			error = "media IP_TOS: " + std::string(std::strerror(errno));
			::close(fd);
			return -1;
		}
	}
	if (!config.interface_name.empty() && ::setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
		config.interface_name.c_str(), config.interface_name.size() + 1) != 0)
	{
		error = "SO_BINDTODEVICE: " + std::string(std::strerror(errno));
		::close(fd);
		return -1;
	}
	const int flags = ::fcntl(fd, F_GETFL, 0);
	if (flags >= 0)
		(void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	if (::inet_pton(AF_INET, config.console_address.c_str(), &address.sin_addr) != 1)
	{
		error = "invalid console address";
		::close(fd);
		return -1;
	}
	if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
	{
		error = "bind " + config.console_address + ":" + std::to_string(port) + ": " + std::strerror(errno);
		::close(fd);
		return -1;
	}
	return fd;
}

}

class RuntimeTransport::Impl
{
public:
	~Impl() { stop(); }

	bool start(const RuntimeTransportConfig& config, std::string& error)
	{
		std::lock_guard lifecycle_lock(m_lifecycle_mutex);
		if (m_running.load())
			return true;
		m_config = config;
		m_message_fd = MakeSocket(config, kConsoleMessagePort, false, error);
		if (m_message_fd < 0)
			return false;
		m_video_fd = MakeSocket(config, kConsoleVideoPort, true, error);
		m_audio_fd = MakeSocket(config, kConsoleAudioPort, true, error);
		m_input_fd = MakeSocket(config, kConsoleInputPort, false, error);
		m_command_fd = MakeSocket(config, kConsoleCommandPort, false, error);
		m_unknown_fd = MakeSocket(config, kConsoleUnknownPort, false, error);
		if (m_video_fd < 0 || m_audio_fd < 0 || m_input_fd < 0 ||
			m_command_fd < 0 || m_unknown_fd < 0)
		{
			close_sockets();
			return false;
		}
		if (::inet_pton(AF_INET, config.gamepad_address.c_str(), &m_gamepad_address.sin_addr) != 1)
		{
			error = "invalid GamePad address";
			close_sockets();
			return false;
		}
		m_gamepad_address.sin_family = AF_INET;
		m_stop.store(false);
		m_protocol_ready.store(false);
		m_ready_event.store(false);
		m_disconnected_event.store(false);
		m_video_resync_event.store(false);
		m_command_packets.store(0);
		m_command_retries.store(0);
		m_command_timeouts.store(0);
		m_uvc_uac_replies.store(0);
		m_input_packets.store(0);
		m_battery_charge_valid.store(false);
		m_battery_charge.store(0);
		m_message_packets.store(0);
		m_video_packets_received.store(0);
		m_audio_packets_received.store(0);
		m_unknown_packets_received.store(0);
		m_video_packets_sent.store(0);
		m_audio_packets_sent.store(0);
		m_command_packets_sent.store(0);
		m_send_errors.store(0);
		m_video_resync_requests.store(0);
		m_uic_reply_seen.store(false);
		m_uvc_uac_reply_seen.store(false);
		m_input_seen.store(false);
		m_waiting_for_streaming.store(true);
		m_session_ready_announced = false;
		m_input_snapshot_valid = false;
		m_waiting_stream_state_valid = false;
		m_last_waiting_for_streaming = true;
		m_last_input_report = {};
		m_next_sequence = 0;
		m_clock_origin = std::chrono::steady_clock::now();
		const auto now = std::chrono::steady_clock::now();
		m_command_transactions = {{
			{.query_type = 0, .payload = BuildUicConfigQuery(), .next_due = now},
			{.query_type = 1, .payload = BuildUvcUacQuery(), .next_due = now},
		}};
		m_uic_response.clear();
		m_uvc_uac_response.clear();
		const char* ap_clock = std::getenv("DRCD_AP_TSF_CLOCK");
		std::error_code driver_error;
		const auto clock_driver = m_config.interface_name.empty() ? std::filesystem::path{} :
			std::filesystem::canonical(std::filesystem::path("/sys/class/net") /
				m_config.interface_name / "device/driver", driver_error);
		const bool automatic_ap_clock = !ap_clock && !driver_error &&
			clock_driver.filename() == "rtw89_8852be";
		if (automatic_ap_clock || (ap_clock && std::string_view(ap_clock) == "1"))
		{
			if (!open_ap_tsf_clock(error))
			{
				close_sockets();
				return false;
			}
		}
		else
		{
			open_tsf_file();
			if (m_tsf_fd < 0)
				open_tsf_monitor();
		}
		m_thread = std::thread([this]() { run(); });
		m_running.store(true);
		queue_status("DRC protocol transport listening on UDP 50010/50022/50023; media QoS TID 5");
		return true;
	}

	void stop()
	{
		std::lock_guard lifecycle_lock(m_lifecycle_mutex);
		m_stop.store(true);
		if (m_thread.joinable())
			m_thread.join();
		close_sockets();
		m_running.store(false);
		m_protocol_ready.store(false);
	}

	bool send(RuntimeChannel channel, std::span<const uint8_t> payload, std::string& error)
	{
		if (m_ap_clock && (channel == RuntimeChannel::Audio || channel == RuntimeChannel::Video)
			&& !m_ap_clock->healthy())
		{
			++m_send_errors;
			error = "AP TSF clock unavailable/stale; refusing media with a different clock";
			return false;
		}
		int fd = -1;
		uint16_t port = 0;
		switch (channel)
		{
		case RuntimeChannel::Video: fd = m_video_fd; port = kGamepadVideoPort; break;
		case RuntimeChannel::Audio: fd = m_audio_fd; port = kGamepadAudioPort; break;
		case RuntimeChannel::Command: fd = m_command_fd; port = kGamepadCommandPort; break;
		case RuntimeChannel::Unknown: fd = m_unknown_fd; port = kGamepadUnknownPort; break;
		default: error = "channel is not outbound"; return false;
		}
		if (fd < 0)
		{
			error = "runtime transport is not running";
			return false;
		}
		sockaddr_in destination = m_gamepad_address;
		destination.sin_port = htons(port);
		if (::sendto(fd, payload.data(), payload.size(), 0,
			reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) < 0)
		{
			++m_send_errors;
			error = std::strerror(errno);
			return false;
		}
		if (channel == RuntimeChannel::Video) ++m_video_packets_sent;
		else if (channel == RuntimeChannel::Audio) ++m_audio_packets_sent;
		else if (channel == RuntimeChannel::Command) ++m_command_packets_sent;
		return true;
	}

	std::optional<RuntimePacket> receive()
	{
		std::lock_guard lock(m_queue_mutex);
		if (m_packets.empty())
			return std::nullopt;
		RuntimePacket packet = std::move(m_packets.front());
		m_packets.pop_front();
		return packet;
	}

	std::optional<std::string> consume_status_event()
	{
		std::lock_guard lock(m_queue_mutex);
		if (m_status.empty())
			return std::nullopt;
		std::string status = std::move(m_status.front());
		m_status.pop_front();
		return status;
	}

	RuntimeTransportStats stats() const
	{
		return {
			.running = m_running.load(),
			.protocol_ready = m_protocol_ready.load(),
			.waiting_for_streaming = m_waiting_for_streaming.load(),
			.battery_charge_valid = m_battery_charge_valid.load(),
			.battery_charge = m_battery_charge.load(),
			.command_packets_received = m_command_packets.load(),
			.command_retries = m_command_retries.load(),
			.command_timeouts = m_command_timeouts.load(),
			.uvc_uac_replies = m_uvc_uac_replies.load(),
			.input_packets_received = m_input_packets.load(),
			.message_packets_received = m_message_packets.load(),
			.video_packets_received = m_video_packets_received.load(),
			.audio_packets_received = m_audio_packets_received.load(),
			.unknown_packets_received = m_unknown_packets_received.load(),
			.video_packets_sent = m_video_packets_sent.load(),
			.audio_packets_sent = m_audio_packets_sent.load(),
			.command_packets_sent = m_command_packets_sent.load(),
			.send_errors = m_send_errors.load(),
			.video_resync_requests = m_video_resync_requests.load(),
		};
	}

	uint32_t timestamp_us() const
	{
		if (m_ap_clock)
			return m_ap_clock->timestamp();
		if (m_tsf_fd >= 0)
		{
			uint64_t tsf = 0;
			if (::pread(m_tsf_fd, &tsf, sizeof(tsf), 0) == sizeof(tsf))
				return static_cast<uint32_t>(tsf);
		}
		// Publish/read the hardware value and its host time as one sample.
		// Independent atomics can combine values from two different packets.
		std::lock_guard clock_lock(m_clock_mutex);
		const uint64_t sampled_tsf = m_monitor_tsf;
		const int64_t sampled_at = m_monitor_tsf_sampled_at;
		if (sampled_tsf != 0 && sampled_at != 0)
		{
			const int64_t elapsed = std::max<int64_t>(0, SteadyMicroseconds() - sampled_at);
			return static_cast<uint32_t>(sampled_tsf + static_cast<uint64_t>(elapsed));
		}
		using namespace std::chrono;
		return static_cast<uint32_t>(duration_cast<microseconds>(steady_clock::now() - m_clock_origin).count());
	}

	void report_status(std::string status) { queue_status(std::move(status)); }

	bool consume_ready_event() { return m_ready_event.exchange(false); }
	bool consume_disconnected_event() { return m_disconnected_event.exchange(false); }
	bool consume_video_resync_event() { return m_video_resync_event.exchange(false); }

private:
	void run()
	{
		using namespace std::chrono;
		auto last_protocol_packet = steady_clock::time_point{};
		auto next_clock_sample = steady_clock::now();
		bool clock_failure_reported = false;
		while (!m_stop.load())
		{
			if (m_ap_clock && steady_clock::now() >= next_clock_sample)
			{
				const bool healthy = m_ap_clock->update();
				if (!healthy && !clock_failure_reported)
					queue_status("AP TSF register sample failed; media paused (no RX-clock fallback)");
				else if (healthy && clock_failure_reported)
					queue_status("AP TSF register sampling recovered");
				clock_failure_reported = !healthy;
				next_clock_sample = steady_clock::now() + milliseconds(100);
			}
			std::array<pollfd, 7> fds{{
				{m_tsf_monitor_fd, POLLIN, 0},
				{m_message_fd, POLLIN, 0},
				{m_video_fd, POLLIN, 0},
				{m_audio_fd, POLLIN, 0},
				{m_input_fd, POLLIN, 0},
				{m_command_fd, POLLIN, 0},
				{m_unknown_fd, POLLIN, 0},
			}};
			if (::poll(fds.data(), fds.size(), 100) > 0)
			{
				for (const auto& pfd : fds)
				{
					if ((pfd.revents & POLLIN) == 0)
						continue;
					if (pfd.fd == m_tsf_monitor_fd)
					{
						drain_tsf_monitor();
						continue;
					}
					RuntimeChannel channel = RuntimeChannel::Command;
					if (pfd.fd == m_message_fd) channel = RuntimeChannel::Message;
					else if (pfd.fd == m_video_fd) channel = RuntimeChannel::Video;
					else if (pfd.fd == m_audio_fd) channel = RuntimeChannel::Audio;
					else if (pfd.fd == m_input_fd) channel = RuntimeChannel::Input;
					else if (pfd.fd == m_unknown_fd) channel = RuntimeChannel::Unknown;
					if (receive_one(pfd.fd, channel))
					{
						last_protocol_packet = steady_clock::now();
					}
				}
			}

			const auto now = steady_clock::now();
			service_command(m_command_transactions[0], now, m_uic_reply_seen.load());
			service_command(m_command_transactions[1], now, false);
			if (m_protocol_ready.load() && last_protocol_packet != steady_clock::time_point{} &&
				now - last_protocol_packet > seconds(3))
			{
				m_protocol_ready.store(false);
				m_session_ready_announced = false;
				m_input_seen.store(false);
				m_uvc_uac_reply_seen.store(false);
				m_waiting_for_streaming.store(true);
				m_disconnected_event.store(true);
				queue_status("DRC protocol traffic timed out");
			}
		}
	}

	bool receive_one(int fd, RuntimeChannel channel)
	{
		std::array<uint8_t, 4096> buffer{};
		sockaddr_in source{};
		socklen_t source_size = sizeof(source);
		const ssize_t count = ::recvfrom(fd, buffer.data(), buffer.size(), 0,
			reinterpret_cast<sockaddr*>(&source), &source_size);
		if (count <= 0)
			return false;
		if (source.sin_addr.s_addr != m_gamepad_address.sin_addr.s_addr)
			return false;

		std::vector<uint8_t> payload(buffer.begin(), buffer.begin() + count);
		if (channel == RuntimeChannel::Command)
		{
			if (!handle_command(payload))
				return false;
			++m_command_packets;
		}
		else if (channel == RuntimeChannel::Input)
		{
			if (payload.size() != 128)
				return false;
			++m_input_packets;
			monitor_input(payload);
		}
		else if (channel == RuntimeChannel::Message)
		{
			++m_message_packets;
			if (payload.size() == 4 && payload[0] == 1 && payload[1] == 0 &&
				payload[2] == 0 && payload[3] == 0)
			{
				m_video_resync_event.store(true);
				const uint64_t count = ++m_video_resync_requests;
				if (count == 1 || count % 100 == 0)
					queue_status("GamePad video IDR resync requests: " + std::to_string(count));
			}
		}
		else
		{
			std::atomic_uint64_t* counter = &m_unknown_packets_received;
			const char* name = "unknown-service";
			if (channel == RuntimeChannel::Video)
			{
				counter = &m_video_packets_received;
				name = "video-service";
			}
			else if (channel == RuntimeChannel::Audio)
			{
				counter = &m_audio_packets_received;
				name = "audio-service";
			}
			const uint64_t received = ++*counter;
			if (received == 1)
				queue_status(std::string("GamePad returned first ") + name + " packet (" +
					std::to_string(payload.size()) + " bytes)");
		}

		{
			std::lock_guard lock(m_queue_mutex);
			if (m_packets.size() >= 1024)
				m_packets.pop_front();
			m_packets.push_back({channel, std::move(payload)});
		}
		mark_protocol_ready(channel);
		return true;
	}

	bool handle_command(const std::vector<uint8_t>& packet)
	{
		if (packet.size() < kCommandHeaderSize)
			return false;
		const uint16_t type = ReadLe16(packet.data());
		const uint16_t query_type = ReadLe16(packet.data() + 2);
		const uint16_t payload_size = ReadLe16(packet.data() + 4);
		const uint16_t sequence = ReadLe16(packet.data() + 6);
		if (type > 3 || query_type > 2 || packet.size() != kCommandHeaderSize + payload_size)
			return false;
		if (type == 2)
		{
			const auto ack = BuildCommand(3, query_type, sequence, {});
			std::string error;
			if (!send(RuntimeChannel::Command, ack, error))
				queue_status("Command reply-ack send failed: " + error);
		}

		CommandTransaction* transaction = find_command_transaction(query_type, sequence);
		if (transaction == nullptr)
		{
			queue_status("Command rx " + std::string(CommandPacketTypeName(type)) +
				" query=" + std::string(CommandQueryTypeName(query_type)) +
				" seq=" + std::to_string(sequence) + " ignored (not outstanding)");
			return true;
		}

		const auto now = std::chrono::steady_clock::now();
		const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
			now - transaction->last_sent).count();
		if (type == 1)
		{
			if (payload_size != 0)
			{
				queue_status("Command query-ack query=" +
					std::string(CommandQueryTypeName(query_type)) + " seq=" +
					std::to_string(sequence) + " rejected: non-empty payload");
				return true;
			}
			transaction->stage = CommandStage::AwaitingReply;
			queue_status("Command rx query-ack query=" +
				std::string(CommandQueryTypeName(query_type)) + " seq=" +
				std::to_string(sequence) + " latency_ms=" + std::to_string(latency));
		}
		else if (type == 2)
		{
			std::string reason;
			const std::span<const uint8_t> response(packet.data() + kCommandHeaderSize,
				payload_size);
			if (!ValidateCommandReply(query_type, response, reason))
			{
				queue_status("Command reply query=" +
					std::string(CommandQueryTypeName(query_type)) + " seq=" +
					std::to_string(sequence) + " rejected: " + reason);
				return true;
			}

			queue_status("Command rx reply query=" +
				std::string(CommandQueryTypeName(query_type)) + " seq=" +
				std::to_string(sequence) + " payload=" + std::to_string(payload_size) +
				" latency_ms=" + std::to_string(latency) + " retry=" +
				std::to_string(transaction->attempts - 1));
			transaction->stage = CommandStage::Idle;
			transaction->next_due = now + kCommandTimeout;
			if (query_type == 0)
			{
				m_uic_response.assign(response.begin(), response.end());
				if (!m_uic_reply_seen.exchange(true))
					queue_status("UIC configuration synchronized");
			}
			else if (query_type == 1)
			{
				m_uvc_uac_response.assign(response.begin(), response.end());
				SynchronizeUvcUacRequest(transaction->payload, response);
				++m_uvc_uac_replies;
				if (!m_uvc_uac_reply_seen.exchange(true))
					queue_status("UVC/UAC keepalive synchronized; media may start");
				maybe_mark_session_ready();
			}
		}
		return true;
	}

	CommandTransaction* find_command_transaction(uint16_t query_type, uint16_t sequence)
	{
		for (auto& transaction : m_command_transactions)
		{
			if (transaction.query_type == query_type && transaction.sequence == sequence &&
				transaction.stage != CommandStage::Idle)
				return &transaction;
		}
		return nullptr;
	}

	void send_command_attempt(CommandTransaction& transaction,
		std::chrono::steady_clock::time_point now)
	{
		if (transaction.stage == CommandStage::Idle)
		{
			transaction.sequence = m_next_sequence++;
			transaction.attempts = 0;
			transaction.first_sent = now;
		}
		++transaction.attempts;
		transaction.stage = CommandStage::AwaitingAck;
		transaction.last_sent = now;
		transaction.next_due = now + kCommandTimeout;
		const auto packet = BuildCommand(0, transaction.query_type, transaction.sequence,
			transaction.payload);
		std::string error;
		if (!send(RuntimeChannel::Command, packet, error) && !error.empty())
			queue_status("DRC command send failed: " + error);
		queue_status("Command tx query=" +
			std::string(CommandQueryTypeName(transaction.query_type)) + " seq=" +
			std::to_string(transaction.sequence) + " payload=" +
			std::to_string(transaction.payload.size()) + " retry=" +
			std::to_string(transaction.attempts - 1));
	}

	void service_command(CommandTransaction& transaction,
		std::chrono::steady_clock::time_point now, bool completed_permanently)
	{
		if (completed_permanently)
			return;
		if (transaction.stage == CommandStage::Idle)
		{
			if (now >= transaction.next_due)
				send_command_attempt(transaction, now);
			return;
		}
		if (now < transaction.next_due)
			return;
		if (transaction.attempts >= kCommandMaxAttempts)
		{
			++m_command_timeouts;
			queue_status("Command timeout query=" +
				std::string(CommandQueryTypeName(transaction.query_type)) + " seq=" +
				std::to_string(transaction.sequence) + " attempts=" +
				std::to_string(transaction.attempts));
			transaction.stage = CommandStage::Idle;
			transaction.next_due = now + kCommandTimeout;
			return;
		}
		++m_command_retries;
		send_command_attempt(transaction, now);
	}

	void mark_protocol_ready(RuntimeChannel channel)
	{
		if (!m_protocol_ready.exchange(true))
			queue_status(channel == RuntimeChannel::Input
				? "DRC protocol active; receiving GamePad input"
				: "DRC protocol traffic active");
		if (channel == RuntimeChannel::Input)
		{
			m_input_seen.store(true);
			maybe_mark_session_ready();
		}
	}

	void maybe_mark_session_ready()
	{
		if (m_session_ready_announced || !m_input_seen.load() || !m_uvc_uac_reply_seen.load())
			return;
		m_session_ready_announced = true;
		m_ready_event.store(true);
		queue_status("DRC session ready: input active and UVC/UAC keepalive synchronized");
	}

	void monitor_input(std::span<const uint8_t> packet)
	{
		const InputSnapshot current = DecodeInput(packet);
		const auto now = std::chrono::steady_clock::now();
		m_waiting_for_streaming.store(current.waiting_for_streaming);
		m_battery_charge.store(current.battery_charge);
		m_battery_charge_valid.store(true);
		if (!m_waiting_stream_state_valid ||
			current.waiting_for_streaming != m_last_waiting_for_streaming)
		{
			queue_status(std::string("GamePad streaming state: ") +
				(current.waiting_for_streaming ? "waiting" : "active"));
			m_last_waiting_for_streaming = current.waiting_for_streaming;
			m_waiting_stream_state_valid = true;
		}
		const bool buttons_changed = !m_input_snapshot_valid ||
			current.buttons != m_last_input_snapshot.buttons;
		bool sticks_changed = !m_input_snapshot_valid;
		if (m_input_snapshot_valid)
		{
			for (size_t i = 0; i < current.sticks.size(); ++i)
			{
				const int delta = static_cast<int>(current.sticks[i]) -
					static_cast<int>(m_last_input_snapshot.sticks[i]);
				if (std::abs(delta) >= kStickLogThreshold)
				{
					sticks_changed = true;
					break;
				}
			}
		}

		if (buttons_changed || (sticks_changed &&
			(now - m_last_input_report >= kStickLogInterval)))
		{
			queue_status(FormatInput(current));
			m_last_input_snapshot = current;
			m_last_input_report = now;
			m_input_snapshot_valid = true;
		}
	}

	void queue_status(std::string value)
	{
		std::lock_guard lock(m_queue_mutex);
		m_status.push_back(std::move(value));
	}

	bool open_ap_tsf_clock(std::string& error)
	{
		const std::filesystem::path device = std::filesystem::path("/sys/class/net") / m_config.interface_name;
		std::error_code ec;
		const auto driver = std::filesystem::canonical(device / "device/driver", ec);
		if (ec || driver.filename() != "rtw89_8852be")
		{
			error = "AP TSF clock option requires verified rtw89_8852be bank 0 port 0";
			return false;
		}
		const auto phy = std::filesystem::canonical(device / "phy80211", ec);
		if (ec)
		{
			error = "cannot resolve AP PHY for TSF register reads";
			return false;
		}
		const auto path = (std::filesystem::path("/sys/kernel/debug/ieee80211") /
			phy.filename() / "rtw89/read_reg").string();
		m_ap_clock = std::make_unique<ApTsfClock>([path]() { return ReadApPortTsf(path); });
		if (!m_ap_clock->update())
		{
			error = "cannot sample AP TSF registers; check root/debugfs access (no fallback)";
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		if (!m_ap_clock->update())
		{
			error = "AP port-0 TSF is not advancing reliably; refusing clock selection";
			return false;
		}
		queue_status("Media clock: actual RTL8852BE AP TSF bank=0 port=0; "
			"100ms register sampling, no RX-counter offset; do not run register probes concurrently");
		return true;
	}

	void open_tsf_file()
	{
		std::vector<std::string> paths;
		if (const char* override_path = std::getenv("DRCD_TSF_FILE"); override_path && *override_path)
			paths.emplace_back(override_path);
		paths.emplace_back("/sys/class/net/" + m_config.interface_name + "/device/tsf");
		paths.emplace_back("/sys/class/net/" + m_config.interface_name + "/tsf");
		for (const auto& path : paths)
		{
			m_tsf_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
			if (m_tsf_fd >= 0)
			{
				queue_status("Media clock: hardware Wi-Fi TSF from " + path);
				return;
			}
		}
	}

	void open_tsf_monitor()
	{
		if (m_config.tsf_monitor_interface.empty())
		{
			queue_status("Media clock: transport-relative fallback (hardware Wi-Fi TSF unavailable)");
			return;
		}
		const unsigned int interface_index = ::if_nametoindex(m_config.tsf_monitor_interface.c_str());
		if (interface_index == 0)
		{
			queue_status("Media clock: TSF monitor " + m_config.tsf_monitor_interface +
				" unavailable; using transport-relative fallback");
			return;
		}

		m_tsf_monitor_fd = ::socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
			htons(ETH_P_ALL));
		if (m_tsf_monitor_fd < 0)
		{
			queue_status("Media clock: could not open TSF monitor " +
				m_config.tsf_monitor_interface + ": " + std::strerror(errno));
			return;
		}
		sockaddr_ll address{};
		address.sll_family = AF_PACKET;
		address.sll_protocol = htons(ETH_P_ALL);
		address.sll_ifindex = static_cast<int>(interface_index);
		if (::bind(m_tsf_monitor_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
		{
			queue_status("Media clock: could not bind TSF monitor " +
				m_config.tsf_monitor_interface + ": " + std::strerror(errno));
			::close(m_tsf_monitor_fd);
			m_tsf_monitor_fd = -1;
			return;
		}
		queue_status("Media clock: waiting for hardware TSF on " +
			m_config.tsf_monitor_interface);
	}

	void drain_tsf_monitor()
	{
		std::array<uint8_t, 8192> packet{};
		for (;;)
		{
			const ssize_t size = ::recv(m_tsf_monitor_fd, packet.data(), packet.size(), 0);
			if (size < 0)
			{
				if (errno == EINTR)
					continue;
				break;
			}
			if (size == 0)
				break;
			const auto tsf = ReadRadiotapTsf(
				std::span<const uint8_t>(packet.data(), static_cast<size_t>(size)));
			if (!tsf.has_value())
				continue;
			bool first_sample;
			{
				std::lock_guard clock_lock(m_clock_mutex);
				first_sample = m_monitor_tsf == 0;
				m_monitor_tsf = *tsf;
				m_monitor_tsf_sampled_at = SteadyMicroseconds();
			}
			if (first_sample)
				queue_status("Media clock synchronized to hardware TSF=" + std::to_string(*tsf));
		}
	}

	void close_sockets()
	{
		for (int* fd : {&m_message_fd, &m_video_fd, &m_audio_fd, &m_input_fd,
			&m_command_fd, &m_unknown_fd})
		{
			if (*fd >= 0)
				::close(*fd);
			*fd = -1;
		}
		if (m_tsf_fd >= 0)
			::close(m_tsf_fd);
		m_tsf_fd = -1;
		if (m_tsf_monitor_fd >= 0)
			::close(m_tsf_monitor_fd);
		m_tsf_monitor_fd = -1;
		m_ap_clock.reset();
		std::lock_guard clock_lock(m_clock_mutex);
		m_monitor_tsf = 0;
		m_monitor_tsf_sampled_at = 0;
	}

	RuntimeTransportConfig m_config;
	sockaddr_in m_gamepad_address{};
	int m_message_fd = -1;
	int m_input_fd = -1;
	int m_command_fd = -1;
	int m_video_fd = -1;
	int m_audio_fd = -1;
	int m_unknown_fd = -1;
	int m_tsf_fd = -1;
	int m_tsf_monitor_fd = -1;
	std::unique_ptr<ApTsfClock> m_ap_clock;
	mutable std::mutex m_clock_mutex;
	uint64_t m_monitor_tsf = 0;
	int64_t m_monitor_tsf_sampled_at = 0;
	std::mutex m_lifecycle_mutex;
	std::thread m_thread;
	std::atomic_bool m_stop{false};
	std::atomic_bool m_running{false};
	std::atomic_bool m_protocol_ready{false};
	std::atomic_bool m_ready_event{false};
	std::atomic_bool m_disconnected_event{false};
	std::atomic_bool m_video_resync_event{false};
	std::atomic_uint64_t m_command_packets{0};
	std::atomic_uint64_t m_command_retries{0};
	std::atomic_uint64_t m_command_timeouts{0};
	std::atomic_uint64_t m_uvc_uac_replies{0};
	std::atomic_uint64_t m_input_packets{0};
	std::atomic_uint64_t m_message_packets{0};
	std::atomic_uint64_t m_video_packets_received{0};
	std::atomic_uint64_t m_audio_packets_received{0};
	std::atomic_uint64_t m_unknown_packets_received{0};
	std::atomic_uint64_t m_video_packets_sent{0};
	std::atomic_uint64_t m_audio_packets_sent{0};
	std::atomic_uint64_t m_command_packets_sent{0};
	std::atomic_uint64_t m_send_errors{0};
	std::atomic_uint64_t m_video_resync_requests{0};
	std::atomic_bool m_uic_reply_seen{false};
	std::atomic_bool m_uvc_uac_reply_seen{false};
	std::atomic_bool m_input_seen{false};
	std::atomic_bool m_battery_charge_valid{false};
	std::atomic_uint8_t m_battery_charge{0};
	std::atomic_bool m_waiting_for_streaming{true};
	bool m_session_ready_announced = false;
	bool m_input_snapshot_valid = false;
	bool m_waiting_stream_state_valid = false;
	bool m_last_waiting_for_streaming = true;
	InputSnapshot m_last_input_snapshot{};
	std::chrono::steady_clock::time_point m_last_input_report{};
	std::chrono::steady_clock::time_point m_clock_origin{};
	uint16_t m_next_sequence = 0;
	std::array<CommandTransaction, 2> m_command_transactions{};
	std::vector<uint8_t> m_uic_response;
	std::vector<uint8_t> m_uvc_uac_response;
	mutable std::mutex m_queue_mutex;
	std::deque<RuntimePacket> m_packets;
	std::deque<std::string> m_status;
};

RuntimeTransport::RuntimeTransport() : m_impl(std::make_unique<Impl>()) {}
RuntimeTransport::~RuntimeTransport() = default;
bool RuntimeTransport::start(const RuntimeTransportConfig& config, std::string& error) { return m_impl->start(config, error); }
void RuntimeTransport::stop() { m_impl->stop(); }
bool RuntimeTransport::send(RuntimeChannel channel, std::span<const uint8_t> payload, std::string& error) { return m_impl->send(channel, payload, error); }
std::optional<RuntimePacket> RuntimeTransport::receive() { return m_impl->receive(); }
std::optional<std::string> RuntimeTransport::consume_status_event() { return m_impl->consume_status_event(); }
bool RuntimeTransport::consume_ready_event() { return m_impl->consume_ready_event(); }
bool RuntimeTransport::consume_disconnected_event() { return m_impl->consume_disconnected_event(); }
bool RuntimeTransport::consume_video_resync_event() { return m_impl->consume_video_resync_event(); }
RuntimeTransportStats RuntimeTransport::stats() const { return m_impl->stats(); }
void RuntimeTransport::report_status(std::string status) { m_impl->report_status(std::move(status)); }
uint32_t RuntimeTransport::timestamp_us() const { return m_impl->timestamp_us(); }
}
