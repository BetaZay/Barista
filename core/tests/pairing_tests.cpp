#include "drh/mac_address.h"
#include "drh/pairing.h"
#include "drh/runtime_transport.h"
#include "drh/session.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

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

uint16_t read_le16(const uint8_t* data)
{
	return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

void write_le16(uint8_t* data, uint16_t value)
{
	data[0] = static_cast<uint8_t>(value);
	data[1] = static_cast<uint8_t>(value >> 8);
}

std::vector<uint8_t> command_packet(uint16_t type, uint16_t query_type,
	uint16_t sequence, std::span<const uint8_t> payload = {})
{
	std::vector<uint8_t> packet(8 + payload.size());
	write_le16(packet.data(), type);
	write_le16(packet.data() + 2, query_type);
	write_le16(packet.data() + 4, static_cast<uint16_t>(payload.size()));
	write_le16(packet.data() + 6, sequence);
	std::copy(payload.begin(), payload.end(), packet.begin() + 8);
	return packet;
}

int bind_udp(const char* address, uint16_t port)
{
	const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
	expect(fd >= 0, "could not create loopback UDP socket");
	const int one = 1;
	(void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in local{};
	local.sin_family = AF_INET;
	local.sin_port = htons(port);
	expect(::inet_pton(AF_INET, address, &local.sin_addr) == 1,
		"could not parse loopback UDP address");
	expect(::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0,
		"could not bind loopback UDP socket");
	return fd;
}

bool receive_and_answer_command(int fd, int timeout_ms, uint16_t wanted_query,
	uint16_t& sequence, bool expect_synchronized_uvc = false)
{
	pollfd descriptor{fd, POLLIN, 0};
	if (::poll(&descriptor, 1, timeout_ms) <= 0)
		return false;
	std::array<uint8_t, 1024> request{};
	sockaddr_in console{};
	socklen_t console_size = sizeof(console);
	const ssize_t size = ::recvfrom(fd, request.data(), request.size(), 0,
		reinterpret_cast<sockaddr*>(&console), &console_size);
	if (size < 8 || read_le16(request.data()) != 0)
		return false;
	const uint16_t query_type = read_le16(request.data() + 2);
	if (query_type != wanted_query)
		return false;
	const uint16_t payload_size = read_le16(request.data() + 4);
	expect(size == 8 + payload_size, "loopback command query payload length mismatch");
	if (query_type == 1)
	{
		expect(payload_size == 48, "UVC/UAC query payload size mismatch");
		expect(request[20] == 0x80 && request[21] == 0x3e,
			"UVC/UAC microphone sample rate must match real-console little endian");
		expect(request[8] == (expect_synchronized_uvc ? 1 : 0),
			"UVC/UAC byte 0 must be zero initially and one after a valid reply");
		if (expect_synchronized_uvc)
		{
			expect(request[12] == 1, "UVC/UAC microphone enabled state was not synchronized");
			expect(request[14] == 0x34 && request[15] == 0x12,
				"UVC/UAC microphone volume was not synchronized");
			expect(request[16] == 0x78 && request[17] == 0x56,
				"UVC/UAC microphone jack volume was not synchronized");
			expect(request[24] == 7 && request[25] == 1,
				"UVC/UAC camera state was not synchronized");
		}
	}
	sequence = read_le16(request.data() + 6);

	const auto ack = command_packet(1, query_type, sequence);
	expect(::sendto(fd, ack.data(), ack.size(), 0,
		reinterpret_cast<sockaddr*>(&console), console_size) == static_cast<ssize_t>(ack.size()),
		"could not send loopback command ACK");
	std::vector<uint8_t> payload(query_type == 0 ? 0x310 : 16);
	if (query_type == 0)
	{
		payload[0] = 0x7e;
		payload[1] = 0x01;
		payload[6] = 0x05;
		payload[7] = 0x06;
		payload[10] = 0x03;
		payload[11] = 0x04;
	}
	else
	{
		payload[0] = 0x34;
		payload[1] = 0x12;
		payload[2] = 0x78;
		payload[3] = 0x56;
		payload[12] = 1;
		payload[13] = 7;
		payload[14] = 1;
	}
	const auto response = command_packet(2, query_type, sequence, payload);
	expect(::sendto(fd, response.data(), response.size(), 0,
		reinterpret_cast<sockaddr*>(&console), console_size) == static_cast<ssize_t>(response.size()),
		"could not send loopback command response");
	return true;
}

void test_runtime_keepalive()
{
	using namespace std::chrono;
	const int command_fd = bind_udp("127.0.0.2", 50123);
	const int input_fd = bind_udp("127.0.0.2", 0);
	barista::drh::RuntimeTransport transport;
	std::string error;
	expect(transport.start({
		.console_address = "127.0.0.1",
		.gamepad_address = "127.0.0.2",
	}, error), error.empty() ? "could not start loopback transport" : error.c_str());

	bool uic_answered = false;
	bool uvc_answered = false;
	uint16_t uic_sequence = 0;
	uint16_t uvc_sequence = 0;
	const auto command_deadline = steady_clock::now() + seconds(2);
	while ((!uic_answered || !uvc_answered) && steady_clock::now() < command_deadline)
	{
		pollfd descriptor{command_fd, POLLIN, 0};
		if (::poll(&descriptor, 1, 100) <= 0)
			continue;
		std::array<uint8_t, 1024> request{};
		sockaddr_in console{};
		socklen_t console_size = sizeof(console);
		const ssize_t size = ::recvfrom(command_fd, request.data(), request.size(), MSG_PEEK,
			reinterpret_cast<sockaddr*>(&console), &console_size);
		if (size < 8)
		{
			(void)::recv(command_fd, request.data(), request.size(), 0);
			continue;
		}
		const uint16_t type = read_le16(request.data());
		const uint16_t query = read_le16(request.data() + 2);
		if (type != 0 || query > 1)
		{
			(void)::recv(command_fd, request.data(), request.size(), 0);
			continue;
		}
		uint16_t sequence = 0;
		if (receive_and_answer_command(command_fd, 0, query, sequence))
		{
			if (query == 0) { uic_answered = true; uic_sequence = sequence; }
			else { uvc_answered = true; uvc_sequence = sequence; }
		}
	}
	expect(uic_answered, "transport did not issue the UIC command");
	expect(uvc_answered, "transport did not issue the UVC/UAC command");
	expect(uic_sequence != uvc_sequence, "concurrent commands reused a sequence ID");
	std::this_thread::sleep_for(milliseconds(20));
	expect(!transport.consume_ready_event(),
		"transport became stream-ready before receiving GamePad input");

	std::array<uint8_t, 128> input{};
	input[0] = 0x12;
	input[1] = 0x34;
	input[83] = 1;
	input[5] = 73;
	sockaddr_in input_destination{};
	input_destination.sin_family = AF_INET;
	input_destination.sin_port = htons(50022);
	expect(::inet_pton(AF_INET, "127.0.0.1", &input_destination.sin_addr) == 1,
		"could not parse input destination");
	expect(::sendto(input_fd, input.data(), input.size(), 0,
		reinterpret_cast<sockaddr*>(&input_destination), sizeof(input_destination)) ==
		static_cast<ssize_t>(input.size()), "could not send loopback HID input");

	const auto ready_deadline = steady_clock::now() + seconds(1);
	bool ready = false;
	while (!(ready = transport.consume_ready_event()) && steady_clock::now() < ready_deadline)
		std::this_thread::sleep_for(milliseconds(5));
	expect(ready, "transport did not gate readiness on input plus UVC/UAC response");
	const auto stats = transport.stats();
	expect(stats.input_packets_received == 1, "transport did not accept loopback HID input");
	expect(stats.uvc_uac_replies >= 1, "transport did not retain the UVC/UAC reply");
	expect(stats.waiting_for_streaming, "HID waiting-for-streaming bit was not decoded");
	expect(stats.battery_charge_valid && stats.battery_charge == 73, "HID battery charge was not decoded");

	bool saw_big_endian_sequence = false;
	while (auto status = transport.consume_status_event())
		saw_big_endian_sequence = saw_big_endian_sequence ||
			status->find("Input: seq=4660") != std::string::npos;
	expect(saw_big_endian_sequence, "HID sequence was not decoded as big-endian");

	bool second_uvc_answered = false;
	uint16_t second_uvc_sequence = 0;
	const auto keepalive_deadline = steady_clock::now() + seconds(2);
	while (!second_uvc_answered && steady_clock::now() < keepalive_deadline)
	{
		uint16_t sequence = 0;
		if (!receive_and_answer_command(command_fd, 100, 1, sequence, true))
			continue;
		if (sequence != uvc_sequence)
		{
			second_uvc_answered = true;
			second_uvc_sequence = sequence;
		}
	}
	expect(second_uvc_answered, "transport did not issue the periodic UVC/UAC keepalive");
	expect(second_uvc_sequence != uvc_sequence, "UVC/UAC keepalive reused its sequence ID");

	transport.stop();
	::close(input_fd);
	::close(command_fd);
}
}

int main()
{
	const auto parsed_mac = barista::drh::MacAddress::parse("f8:54:f6:7a:5c:ae");
	expect(parsed_mac.has_value(), "failed to parse MAC address");
	expect(parsed_mac->to_string() == "f8:54:f6:7a:5c:ae", "MAC to_string mismatch");
	expect(parsed_mac->to_hex_no_separator() == "f854f67a5cae", "MAC to_hex_no_separator mismatch");

	const auto code = barista::drh::derive_code_from_ap_mac(*parsed_mac);
	expect(code.valid(), "derived pairing code must be valid");
	expect(code.numeric() == 2232, "derived pairing numeric code mismatch");
	expect(code.pin_byte() == 0xae, "pin byte mismatch");
	expect(code.symbols_utf8() == "♦ ♦ ♣ ♦", "symbol mapping mismatch");

	const auto ssid = barista::drh::build_pairing_ssid(*parsed_mac, code);
	expect(ssid == "WiiUf854f67a5caf854f67a5cae_STA1", "pairing SSID mismatch");
	expect(ssid.size() == 32, "pairing SSID must be exactly 32 bytes");

	const auto parsed_code = barista::drh::PairingCode::from_numeric(2232);
	expect(parsed_code.has_value(), "from_numeric returned no value");
	expect(parsed_code->pin_byte() == code.pin_byte(), "from_numeric pin mismatch");

	barista::drh::PairingSessionStateMachine state_machine;
	expect(state_machine.state().phase == barista::drh::SessionPhase::Idle, "initial phase should be idle");
	expect(state_machine.start_pairing("wlan0", *parsed_mac, code), "start_pairing should succeed");
	expect(state_machine.state().phase == barista::drh::SessionPhase::Pairing, "phase should be pairing");
	expect(state_machine.complete_pairing(), "complete_pairing should succeed");
	expect(state_machine.state().phase == barista::drh::SessionPhase::Runtime, "phase should be runtime");
	expect(state_machine.mark_gamepad_connected(true), "mark_gamepad_connected should succeed");
	expect(state_machine.state().gamepad_connected, "gamepad should be connected");
	expect(state_machine.stop_pairing(), "stop_pairing should succeed");
	expect(state_machine.state().phase == barista::drh::SessionPhase::Idle, "phase should return to idle");

	test_runtime_keepalive();

	std::cout << "drc_host_tests: ok\n";
	return 0;
}
