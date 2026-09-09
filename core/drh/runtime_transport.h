#pragma once

#include <cstdint>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace barista::drh
{
enum class RuntimeChannel : uint8_t
{
	Message,
	Video,
	Audio,
	Input,
	Command,
	Unknown,
};

struct RuntimePacket
{
	RuntimeChannel channel{};
	std::vector<uint8_t> payload;
};

struct RuntimeTransportConfig
{
	std::string interface_name;
	// Optional monitor-mode interface on the same PHY. Linux radiotap RX
	// headers expose the hardware TSF even when the driver has no sysfs TSF
	// file, which is required for the GamePad media clock.
	std::string tsf_monitor_interface;
	std::string console_address = "192.168.1.10";
	std::string gamepad_address = "192.168.1.11";
};

struct RuntimeTransportStats
{
	bool running = false;
	bool protocol_ready = false;
	bool waiting_for_streaming = true;
	bool battery_charge_valid = false;
	uint8_t battery_charge = 0;
	uint64_t command_packets_received = 0;
	uint64_t command_retries = 0;
	uint64_t command_timeouts = 0;
	uint64_t uvc_uac_replies = 0;
	uint64_t input_packets_received = 0;
	uint64_t message_packets_received = 0;
	uint64_t video_packets_received = 0;
	uint64_t audio_packets_received = 0;
	uint64_t unknown_packets_received = 0;
	uint64_t video_packets_sent = 0;
	uint64_t audio_packets_sent = 0;
	uint64_t command_packets_sent = 0;
	uint64_t send_errors = 0;
	uint64_t video_resync_requests = 0;
};

// Owns the network-facing Wii U side of the DRC application protocol.  The
// AP, WPA and DHCP layers must already be active before start() is called.
// AppHook clients consume received packets and submit encoded
// video/audio through this API; they do not need direct access to wlan0.
class RuntimeTransport
{
public:
	RuntimeTransport();
	~RuntimeTransport();
	RuntimeTransport(const RuntimeTransport&) = delete;
	RuntimeTransport& operator=(const RuntimeTransport&) = delete;

	bool start(const RuntimeTransportConfig& config, std::string& error);
	void stop();
	bool send(RuntimeChannel channel, std::span<const uint8_t> payload, std::string& error,
		std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point{},
		bool* temporary_failure = nullptr);
	std::optional<RuntimePacket> receive();
	std::optional<std::string> consume_status_event();
	bool consume_ready_event();
	bool consume_disconnected_event();
	bool consume_video_resync_event();
	RuntimeTransportStats stats() const;
	void report_status(std::string status);
	// Best available reading of the AP TSF clock, in microseconds. A sysfs TSF
	// exporter is preferred, followed by radiotap samples from the configured
	// monitor interface. It is zero-based only when neither source is available.
	uint32_t timestamp_us() const;

private:
	class Impl;
	std::unique_ptr<Impl> m_impl;
};
}
