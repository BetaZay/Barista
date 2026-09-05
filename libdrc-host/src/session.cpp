#include "drc_host/session.h"

namespace drc_host
{
const PairingSessionState& PairingSessionStateMachine::state() const
{
	return m_state;
}

void PairingSessionStateMachine::reset()
{
	m_state = {};
}

bool PairingSessionStateMachine::start_pairing(const std::string& interface_name, const MacAddress& ap_mac, const PairingCode& code)
{
	if (interface_name.empty() || !code.valid())
		return false;
	if (m_state.phase != SessionPhase::Idle)
		return false;

	m_state.phase = SessionPhase::Pairing;
	m_state.interface_name = interface_name;
	m_state.ap_mac = ap_mac;
	m_state.pairing_code = code;
	m_state.gamepad_connected = false;
	return true;
}

bool PairingSessionStateMachine::complete_pairing()
{
	if (m_state.phase != SessionPhase::Pairing)
		return false;

	m_state.phase = SessionPhase::Runtime;
	return true;
}

bool PairingSessionStateMachine::stop_pairing()
{
	if (m_state.phase == SessionPhase::Idle)
		return false;
	reset();
	return true;
}

bool PairingSessionStateMachine::mark_gamepad_connected(bool connected)
{
	if (m_state.phase != SessionPhase::Runtime)
		return false;
	m_state.gamepad_connected = connected;
	return true;
}
}
