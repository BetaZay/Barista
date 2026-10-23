#include "drh/server/server_core.h"
#include "drh/encoder/media_streamer.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

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

bool starts_with(const std::string& value, const std::string& prefix)
{
	return value.rfind(prefix, 0) == 0;
}

bool contains(const std::string& value, const std::string& needle)
{
	return value.find(needle) != std::string::npos;
}

class FakeBackend final : public barista::drh::SessionBackend
{
public:
	bool channel_search_active = false;
	std::string start_error;
	int start_count = 0;
	bool pairing_channel_search_active() const override { return channel_search_active; }

	barista::drh::BackendResult start_pairing(const barista::drh::PairStartRequest& request) override
	{
		++start_count;
		if (!start_error.empty()) return {false, start_error};
		m_snapshot.phase = "pairing";
		m_snapshot.base_interface = request.interface_name;
		m_snapshot.ap_interface = request.interface_name;
		m_snapshot.using_virtual_ap = false;
		m_snapshot.last_error.clear();
		return {true, "pairing started"};
	}

	barista::drh::BackendResult enter_runtime() override
	{
		if (m_snapshot.phase != "pairing")
		{
			m_snapshot.last_error = "backend is not in pairing phase";
			return {false, m_snapshot.last_error};
		}
		m_snapshot.phase = "runtime";
		m_snapshot.last_error.clear();
		return {true, "runtime active"};
	}

	barista::drh::BackendResult start_runtime(const barista::drh::PairStartRequest& request) override
	{
		++start_count;
		if (!start_error.empty()) return {false, start_error};
		m_snapshot.phase = "runtime";
		m_snapshot.base_interface = request.interface_name;
		m_snapshot.ap_interface = request.interface_name;
		m_snapshot.last_error.clear();
		return {true, "runtime active"};
	}

	barista::drh::BackendResult stop_session() override
	{
		m_snapshot = {};
		return {true, "session stopped"};
	}

	barista::drh::BackendSnapshot snapshot() const override
	{
		return m_snapshot;
	}

	bool consume_pairing_complete_event() override
	{
		const bool pending = m_pairing_complete_event;
		m_pairing_complete_event = false;
		return pending;
	}

	bool consume_gamepad_connected_event() override
	{
		const bool pending = m_connected_event;
		m_connected_event = false;
		return pending;
	}

	void signal_pairing_complete()
	{
		m_pairing_complete_event = true;
	}

	void signal_connected()
	{
		m_connected_event = true;
	}

private:
	barista::drh::BackendSnapshot m_snapshot;
	bool m_pairing_complete_event = false;
	bool m_connected_event = false;
};
}

int main()
{
	{
		std::string media_error;
		expect(barista::drh::MediaStreamer::protocol_self_test(media_error),
			media_error.empty() ? "DRH media protocol self-test failed" : media_error.c_str());
	}

	barista::drh::ServerCore core(std::make_unique<FakeBackend>());

	{
		const auto response = core.handle_line("status");
		expect(response.ok, "status should succeed");
		expect(!response.shutdown_requested, "status should not request shutdown");
		expect(starts_with(response.payload, "OK status"), "status payload prefix mismatch");
		expect(contains(response.payload, "phase=idle"), "status phase should be idle");
	}

	{
		const auto response = core.handle_line("pair-start wlan0 f8:54:f6:7a:5c:ae 2232");
		expect(response.ok, "pair-start should succeed");
		expect(starts_with(response.payload, "OK pairing started"), "pair-start payload prefix mismatch");
		expect(contains(response.payload, "phase=pairing"), "phase should be pairing");
		expect(contains(response.payload, "pair_code=2232"), "pair code should be 2232");
	}

	{
		const auto response = core.handle_line("runtime-start");
		expect(response.ok, "runtime-start should succeed");
		expect(contains(response.payload, "phase=runtime"), "phase should be runtime");
	}

	{
		const auto response = core.handle_line("set-connected 1");
		expect(response.ok, "set-connected 1 should succeed");
		expect(contains(response.payload, "connected=1"), "connected flag should be 1");
	}

	{
		const auto response = core.handle_line("pair-stop");
		expect(response.ok, "pair-stop should succeed");
		expect(contains(response.payload, "phase=idle"), "phase should return to idle");
	}

	{
		const auto response = core.handle_line("pair-start wlan0 notamac 2232");
		expect(!response.ok, "invalid MAC should fail");
		expect(starts_with(response.payload, "ERR invalid MAC address"), "invalid MAC error mismatch");
	}

	{
		const auto response = core.handle_line("shutdown");
		expect(response.ok, "shutdown should succeed");
		expect(response.shutdown_requested, "shutdown flag should be set");
	}

	{
		auto backend = std::make_unique<FakeBackend>();
		auto* backend_ptr = backend.get();
		barista::drh::ServerCore automatic_core(std::move(backend));
		expect(automatic_core.handle_line("pair-start wlan0 f8:54:f6:7a:5c:ae 2232").ok,
			"automatic pair-start should succeed");
		backend_ptr->signal_pairing_complete();
		std::ostringstream events;
		auto* previous_output = std::cout.rdbuf(events.rdbuf());
		automatic_core.process_backend_events();
		std::cout.rdbuf(previous_output);
		expect(contains(events.str(), "BARISTA_EVENT|info|pairing|PAIRING_SUCCEEDED"),
			"pairing completion should emit a structured diagnostic event");
		const auto response = automatic_core.handle_line("status");
		expect(contains(response.payload, "phase=runtime"),
			"WPS completion event should enter runtime automatically");
	}

	{
		auto backend = std::make_unique<FakeBackend>();
		auto* radio = backend.get();
		barista::drh::ServerCore automatic_core(std::move(backend));
		barista::drh::AutomaticCycleConfig config;
		config.request = {"wlan-test", *barista::drh::MacAddress::parse("00:11:22:33:44:55"),
			*barista::drh::PairingCode::from_numeric(123)};
		config.start_in_pairing = true;
		config.pairing_duration = std::chrono::seconds(0);
		radio->channel_search_active = true;
		automatic_core.start_automatic(config);
		for (int channel = 0; channel < 9; ++channel)
		{
			automatic_core.process_backend_events();
			expect(automatic_core.state().phase == barista::drh::SessionPhase::Pairing,
				"ordinary timeout must not interrupt an active channel search");
		}
		radio->channel_search_active = false;
		automatic_core.process_backend_events();
		expect(automatic_core.state().phase == barista::drh::SessionPhase::Runtime,
			"a completed search must still time out and check for an already paired GamePad");

		config.pairing_duration = std::chrono::seconds(20);
		radio->channel_search_active = true;
		automatic_core.start_automatic(config);
		automatic_core.process_backend_events();
		radio->channel_search_active = false;
		automatic_core.process_backend_events();
		expect(automatic_core.state().phase == barista::drh::SessionPhase::Pairing,
			"locking onto pairing activity must retain time to finish authentication");
		radio->signal_pairing_complete();
		automatic_core.process_backend_events();
		expect(automatic_core.state().phase == barista::drh::SessionPhase::Runtime,
			"successful WPS must transition immediately without waiting for the search timeout");
	}

	for (const auto* command : {"pair-stop", "runtime-stop", "shutdown"})
	{
		for (const bool failed_start : {false, true})
		{
			auto backend = std::make_unique<FakeBackend>();
			auto* radio = backend.get();
			if (failed_start) radio->start_error = "runtime AP failed";
			barista::drh::ServerCore automatic_core(std::move(backend));
			barista::drh::AutomaticCycleConfig config;
			config.request = {"wlan-test", *barista::drh::MacAddress::parse("00:11:22:33:44:55"),
				*barista::drh::PairingCode::from_numeric(123)};
			config.start_in_pairing = true;
			config.pairing_duration = std::chrono::seconds(0);
			config.check_duration = std::chrono::seconds(0);
			automatic_core.start_automatic(config);
			(void)automatic_core.handle_line(command);
			const auto start_count = radio->start_count;
			for (int poll = 0; poll < 3; ++poll) automatic_core.process_backend_events();
			expect(radio->start_count == start_count,
				"explicit stop must cancel automatic retries even when a failed start left the backend idle");
			expect(automatic_core.state().phase == barista::drh::SessionPhase::Idle,
				"stopped automatic sessions must remain idle");
		}
	}

	{
		auto backend = std::make_unique<FakeBackend>();
		auto* radio = backend.get();
		barista::drh::ServerCore automatic_core(std::move(backend));
		barista::drh::AutomaticCycleConfig config;
		config.request = {"wlan-test", *barista::drh::MacAddress::parse("00:11:22:33:44:55"),
			*barista::drh::PairingCode::from_numeric(123)};
		config.start_in_pairing = true;
		automatic_core.start_automatic(config);
		radio->signal_pairing_complete();
		automatic_core.process_backend_events();
		automatic_core.start_automatic(config);
		expect(automatic_core.state().phase == barista::drh::SessionPhase::Pairing,
			"a new sync request must not inherit the previous session's post-pair suppression");
	}

	{
		auto backend = std::make_unique<FakeBackend>();
		auto* radio = backend.get();
		radio->start_error = "wireless regulatory domain marks the selected AP channel no-IR";
		barista::drh::ServerCore automatic_core(std::move(backend));
		barista::drh::AutomaticCycleConfig config;
		config.request = {"wlan-test", *barista::drh::MacAddress::parse("00:11:22:33:44:55"),
			*barista::drh::PairingCode::from_numeric(123)};
		config.pairing_enabled = false;
		std::ostringstream events;
		auto* previous_output = std::cout.rdbuf(events.rdbuf());
		automatic_core.start_automatic(config);
		std::cout.rdbuf(previous_output);
		expect(contains(events.str(), "BARISTA_EVENT|error|wifi|AP_REGULATORY_BLOCKED"),
			"runtime startup failures must report their actionable diagnostic");

		radio->start_error = "no saved credentials for AP 00:11:22:33:44:55";
		config.pairing_enabled = true;
		events.str("");
		previous_output = std::cout.rdbuf(events.rdbuf());
		automatic_core.start_automatic(config);
		std::cout.rdbuf(previous_output);
		const auto pairing = events.str().find("PAIRING_CYCLE_STARTED");
		expect(pairing != std::string::npos, "missing saved credentials should start pairing");
		expect(!contains(events.str().substr(0, pairing), "BARISTA_EVENT|error|"),
			"an initial discovery check with no credentials must not report a spurious error");
	}

	std::cout << "drcd_core_tests: ok\n";
	return 0;
}
