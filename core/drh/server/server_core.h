#pragma once

#include "drh/server/session_backend.h"
#include "drh/session.h"

#include <memory>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace barista::drh
{
struct CommandResponse
{
	bool ok = false;
	bool shutdown_requested = false;
	std::string payload;
};

struct AutomaticCycleConfig
{
	PairStartRequest request;
	std::chrono::seconds check_duration{5};
	std::chrono::seconds pairing_duration{20};
	std::chrono::seconds post_pair_grace_duration{60};
	bool pairing_enabled = true;
	bool start_in_pairing = false;
};

class ServerCore
{
public:
	ServerCore();
	explicit ServerCore(std::unique_ptr<SessionBackend> backend);

	CommandResponse handle_line(std::string_view line);
	void start_automatic(AutomaticCycleConfig config);
	void process_backend_events();
	const barista::drh::PairingSessionState& state() const;

private:
	std::unique_ptr<SessionBackend> m_backend;
	barista::drh::PairingSessionStateMachine m_state_machine;
	std::optional<AutomaticCycleConfig> m_automatic;
	std::chrono::steady_clock::time_point m_phase_deadline{};
	std::chrono::steady_clock::time_point m_pairing_suppressed_until{};

	void start_pairing_cycle();
	void start_check_cycle();
};
}
