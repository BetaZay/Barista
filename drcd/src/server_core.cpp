#include "drcd/server_core.h"

#include "drc_host/mac_address.h"
#include "drc_host/pairing.h"

#include <sstream>
#include <iostream>
#include <string>
#include <vector>

namespace drcd
{
namespace
{
struct CommandResult
{
	bool ok = false;
	std::string message;
	bool shutdown_requested = false;
};

std::vector<std::string> SplitWhitespace(std::string_view line)
{
	std::vector<std::string> tokens;
	std::string current;
	for (char c : line)
	{
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
		{
			if (!current.empty())
			{
				tokens.emplace_back(std::move(current));
				current.clear();
			}
			continue;
		}
		current.push_back(c);
	}
	if (!current.empty())
		tokens.emplace_back(std::move(current));
	return tokens;
}

std::string PhaseToString(drc_host::SessionPhase phase)
{
	switch (phase)
	{
	case drc_host::SessionPhase::Idle:
		return "idle";
	case drc_host::SessionPhase::Pairing:
		return "pairing";
	case drc_host::SessionPhase::Runtime:
		return "runtime";
	default:
		return "unknown";
	}
}

std::string BuildStatusResponse(bool ok, const std::string& message, const drc_host::PairingSessionState& state, const BackendSnapshot& backend)
{
	std::ostringstream out;
	out << (ok ? "OK " : "ERR ") << message << "\n";
	out << "phase=" << PhaseToString(state.phase) << "\n";
	out << "interface=" << (state.interface_name.empty() ? "-" : state.interface_name) << "\n";
	out << "ap_mac=" << (state.ap_mac.has_value() ? state.ap_mac->to_string() : "-") << "\n";
	out << "pair_code=" << (state.pairing_code.has_value() ? std::to_string(state.pairing_code->numeric()) : "-") << "\n";
	out << "pair_symbols=" << (state.pairing_code.has_value() ? state.pairing_code->symbols_utf8() : "-") << "\n";
	out << "connected=" << (state.gamepad_connected ? "1" : "0") << "\n";
	out << "battery_available=" << (backend.battery_charge_valid ? "1" : "0") << "\n";
	if (backend.battery_charge_valid)
		out << "battery=" << static_cast<unsigned int>(backend.battery_charge) << "\n";
	out << "backend_phase=" << (backend.phase.empty() ? "-" : backend.phase) << "\n";
	out << "backend_base_interface=" << (backend.base_interface.empty() ? "-" : backend.base_interface) << "\n";
	out << "backend_ap_interface=" << (backend.ap_interface.empty() ? "-" : backend.ap_interface) << "\n";
	out << "backend_virtual_ap=" << (backend.using_virtual_ap ? "1" : "0") << "\n";
	out << "backend_last_error=" << (backend.last_error.empty() ? "-" : backend.last_error) << "\n";
	return out.str();
}

CommandResult ExecuteCommand(const std::vector<std::string>& tokens, drc_host::PairingSessionStateMachine& state_machine, SessionBackend& backend)
{
	CommandResult result;
	if (tokens.empty())
	{
		result.message = "empty command";
		return result;
	}

	const std::string& cmd = tokens[0];
	if (cmd == "status")
	{
		result.ok = true;
		result.message = "status";
		return result;
	}
	if (cmd == "pair-start")
	{
		if (tokens.size() != 4)
		{
			result.message = "usage: pair-start <iface> <ap-mac> <code>";
			return result;
		}

		const auto ap_mac = drc_host::MacAddress::parse(tokens[2]);
		if (!ap_mac.has_value())
		{
			result.message = "invalid MAC address";
			return result;
		}

		int code_value = -1;
		try
		{
			code_value = std::stoi(tokens[3]);
		}
		catch (...)
		{
			result.message = "invalid pairing code";
			return result;
		}

		const auto code = drc_host::PairingCode::from_numeric(static_cast<uint16_t>(code_value));
		if (!code.has_value())
		{
			result.message = "pairing code must use digits 0-3";
			return result;
		}

		if (!state_machine.start_pairing(tokens[1], *ap_mac, *code))
		{
			result.message = "cannot start pairing from current state";
			return result;
		}

		const PairStartRequest request{
			.interface_name = tokens[1],
			.ap_mac = *ap_mac,
			.pairing_code = *code,
		};
		const BackendResult backend_result = backend.start_pairing(request);
		if (!backend_result.ok)
		{
			state_machine.reset();
			result.message = backend_result.message.empty() ? "backend refused pairing start" : backend_result.message;
			return result;
		}

		result.ok = true;
		result.message = backend_result.message.empty() ? "pairing started" : backend_result.message;
		return result;
	}
	if (cmd == "pair-complete" || cmd == "runtime-start")
	{
		if (state_machine.state().phase != drc_host::SessionPhase::Pairing)
		{
			result.message = "cannot transition to runtime from current state";
			return result;
		}

		const BackendResult backend_result = backend.enter_runtime();
		if (!backend_result.ok)
		{
			result.message = backend_result.message.empty() ? "backend failed runtime transition" : backend_result.message;
			return result;
		}

		if (!state_machine.complete_pairing())
		{
			result.message = "cannot transition to runtime from current state";
			return result;
		}
		result.ok = true;
		result.message = backend_result.message.empty() ? "runtime active" : backend_result.message;
		return result;
	}
	if (cmd == "pair-stop" || cmd == "runtime-stop")
	{
		if (state_machine.state().phase == drc_host::SessionPhase::Idle)
		{
			result.message = "already idle";
			return result;
		}

		const BackendResult backend_result = backend.stop_session();
		if (!state_machine.stop_pairing())
		{
			result.message = "already idle";
			return result;
		}

		result.ok = backend_result.ok;
		result.message = backend_result.message.empty() ? (backend_result.ok ? "session stopped" : "backend stop failed") : backend_result.message;
		return result;
	}
	if (cmd == "set-connected")
	{
		if (tokens.size() != 2 || (tokens[1] != "0" && tokens[1] != "1"))
		{
			result.message = "usage: set-connected <0|1>";
			return result;
		}
		const bool connected = tokens[1] == "1";
		if (!state_machine.mark_gamepad_connected(connected))
		{
			result.message = "set-connected requires runtime phase";
			return result;
		}
		result.ok = true;
		result.message = connected ? "gamepad connected" : "gamepad disconnected";
		return result;
	}
	if (cmd == "shutdown")
	{
		(void)backend.stop_session();
		state_machine.reset();
		result.ok = true;
		result.message = "shutdown requested";
		result.shutdown_requested = true;
		return result;
	}

	result.message = "unknown command";
	return result;
}
}

ServerCore::ServerCore()
	: m_backend(create_default_session_backend())
{
}

ServerCore::ServerCore(std::unique_ptr<SessionBackend> backend)
	: m_backend(std::move(backend))
{
	if (!m_backend)
		m_backend = create_default_session_backend();
}

const drc_host::PairingSessionState& ServerCore::state() const
{
	return m_state_machine.state();
}

CommandResponse ServerCore::handle_line(std::string_view line)
{
	const std::vector<std::string> tokens = SplitWhitespace(line);
	const CommandResult result = ExecuteCommand(tokens, m_state_machine, *m_backend);
	CommandResponse response;
	response.ok = result.ok;
	response.shutdown_requested = result.shutdown_requested;
	response.payload = BuildStatusResponse(result.ok, result.message, m_state_machine.state(), m_backend->snapshot());
	return response;
}

void ServerCore::process_backend_events()
{
	while (const auto status = m_backend->consume_status_event())
		std::cout << *status << std::endl;

	if (m_backend->consume_gamepad_connected_event())
	{
		std::cout << "Gamepad found" << std::endl;
		if (m_state_machine.state().phase == drc_host::SessionPhase::Runtime)
			(void)m_state_machine.mark_gamepad_connected(true);
	}
	if (m_backend->consume_gamepad_associated_event() && m_automatic.has_value() &&
		m_state_machine.state().phase == drc_host::SessionPhase::Runtime)
	{
		// WPA/DHCP commonly finish just after the short discovery check. Keep
		// the AP available long enough for the GamePad's DRC sockets to start.
		m_phase_deadline = std::max(m_phase_deadline,
			std::chrono::steady_clock::now() + std::chrono::seconds(20));
		std::cout << "GamePad associated; extending protocol startup grace period" << std::endl;
	}
	if (m_backend->consume_gamepad_disconnected_event() &&
		m_state_machine.state().phase == drc_host::SessionPhase::Runtime)
	{
		std::cout << "Gamepad disconnected; waiting for it to reconnect" << std::endl;
		(void)m_state_machine.mark_gamepad_connected(false);
		if (m_automatic.has_value())
			m_phase_deadline = std::max(
				std::chrono::steady_clock::now() + m_automatic->check_duration,
				m_pairing_suppressed_until);
	}

	if (m_state_machine.state().phase == drc_host::SessionPhase::Pairing &&
		m_backend->consume_pairing_complete_event())
	{
		std::cout << "Gamepad paired" << std::endl;
		if (m_automatic.has_value())
			m_pairing_suppressed_until = std::chrono::steady_clock::now() +
				m_automatic->post_pair_grace_duration;
		const BackendResult backend_result = m_backend->enter_runtime();
		if (backend_result.ok)
		{
			(void)m_state_machine.complete_pairing();
			if (m_automatic.has_value())
				m_phase_deadline = m_pairing_suppressed_until;
		}
		else if (m_automatic.has_value())
		{
			m_phase_deadline = std::chrono::steady_clock::now() + m_automatic->check_duration;
		}
	}

	if (!m_automatic.has_value() || m_state_machine.state().gamepad_connected ||
		std::chrono::steady_clock::now() < m_phase_deadline)
		return;

	if (!m_automatic->pairing_enabled ||
		m_state_machine.state().phase == drc_host::SessionPhase::Pairing ||
		std::chrono::steady_clock::now() < m_pairing_suppressed_until)
		start_check_cycle();
	else
		start_pairing_cycle();
}

void ServerCore::start_automatic(AutomaticCycleConfig config)
{
	m_automatic = std::move(config);
	if (m_automatic->start_in_pairing)
		start_pairing_cycle();
	else
		start_check_cycle();
}

void ServerCore::start_pairing_cycle()
{
	if (!m_automatic.has_value())
		return;
	if (!m_automatic->pairing_enabled)
	{
		start_check_cycle();
		return;
	}
	if (std::chrono::steady_clock::now() < m_pairing_suppressed_until)
	{
		start_check_cycle();
		return;
	}
	(void)m_backend->stop_session();
	m_state_machine.reset();
	if (!m_state_machine.start_pairing(m_automatic->request.interface_name,
		m_automatic->request.ap_mac, m_automatic->request.pairing_code))
		return;
	std::cout << "Running sync with pattern: " << m_automatic->request.pairing_code.symbols_utf8() << std::endl;
	const BackendResult result = m_backend->start_pairing(m_automatic->request);
	if (!result.ok)
		m_state_machine.reset();
	m_phase_deadline = std::chrono::steady_clock::now() + m_automatic->pairing_duration;
}

void ServerCore::start_check_cycle()
{
	if (!m_automatic.has_value())
		return;
	(void)m_backend->stop_session();
	m_state_machine.reset();
	std::cout << "Checking for gamepads" << std::endl;
	if (m_state_machine.start_pairing(m_automatic->request.interface_name,
		m_automatic->request.ap_mac, m_automatic->request.pairing_code))
	{
		const BackendResult result = m_backend->start_runtime(m_automatic->request);
		if (result.ok)
		{
			(void)m_state_machine.complete_pairing();
			m_phase_deadline = std::max(
				std::chrono::steady_clock::now() + m_automatic->check_duration,
				m_pairing_suppressed_until);
			return;
		}
	}
	m_state_machine.reset();
	if (!m_automatic->pairing_enabled)
	{
		m_phase_deadline = std::chrono::steady_clock::now() + m_automatic->check_duration;
		return;
	}
	if (std::chrono::steady_clock::now() < m_pairing_suppressed_until)
	{
		m_phase_deadline = std::chrono::steady_clock::now() + m_automatic->check_duration;
		return;
	}
	start_pairing_cycle();
}
}
