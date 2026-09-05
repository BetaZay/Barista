#pragma once

#include "drc_host/mac_address.h"
#include "drc_host/pairing.h"

#include <optional>
#include <string>

namespace drc_host
{
enum class SessionPhase
{
	Idle,
	Pairing,
	Runtime
};

struct PairingSessionState
{
	SessionPhase phase = SessionPhase::Idle;
	std::string interface_name;
	std::optional<MacAddress> ap_mac;
	std::optional<PairingCode> pairing_code;
	bool gamepad_connected = false;
};

class PairingSessionStateMachine
{
public:
	const PairingSessionState& state() const;

	void reset();
	bool start_pairing(const std::string& interface_name, const MacAddress& ap_mac, const PairingCode& code);
	bool complete_pairing();
	bool stop_pairing();
	bool mark_gamepad_connected(bool connected);

private:
	PairingSessionState m_state;
};
}
