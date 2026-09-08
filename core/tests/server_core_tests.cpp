#include "drh/server/server_core.h"
#include "drh/encoder/media_streamer.h"

#include <cstdlib>
#include <iostream>
#include <memory>
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
	barista::drh::BackendResult start_pairing(const barista::drh::PairStartRequest& request) override
	{
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
		automatic_core.process_backend_events();
		const auto response = automatic_core.handle_line("status");
		expect(contains(response.payload, "phase=runtime"),
			"WPS completion event should enter runtime automatically");
	}

	std::cout << "drcd_core_tests: ok\n";
	return 0;
}
