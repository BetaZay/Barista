#pragma once

#include "drh/mac_address.h"
#include "drh/pairing.h"

#include <memory>
#include <optional>
#include <cstdint>
#include <string>

namespace barista::drh
{
struct PairStartRequest
{
	std::string interface_name;
	barista::drh::MacAddress ap_mac;
	barista::drh::PairingCode pairing_code;
	std::string test_media_path;
	bool test_black_frames = false;
};

struct BackendResult
{
	bool ok = false;
	std::string message;
};

struct BackendSnapshot
{
	std::string phase = "idle";
	std::string base_interface;
	std::string ap_interface;
	bool using_virtual_ap = false;
	bool battery_charge_valid = false;
	uint8_t battery_charge = 0;
	std::string last_error;
};

class SessionBackend
{
public:
	virtual ~SessionBackend() = default;

	virtual BackendResult start_pairing(const PairStartRequest& request) = 0;
	virtual BackendResult start_runtime(const PairStartRequest& request) = 0;
	virtual BackendResult enter_runtime() = 0;
	virtual BackendResult stop_session() = 0;
	virtual BackendSnapshot snapshot() const = 0;
	virtual bool consume_pairing_complete_event() { return false; }
	virtual bool consume_gamepad_connected_event() { return false; }
	// Link association is progress, but is not enough to declare the DRC
	// protocol connected. Automatic mode uses it to keep the AP alive.
	virtual bool consume_gamepad_associated_event() { return false; }
	virtual bool consume_gamepad_disconnected_event() { return false; }
	virtual std::optional<std::string> consume_status_event() { return std::nullopt; }
};

std::unique_ptr<SessionBackend> create_default_session_backend();
}
