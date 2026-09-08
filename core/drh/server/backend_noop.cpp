#include "drh/server/session_backend.h"

namespace barista::drh
{
namespace
{
class NoopBackend final : public SessionBackend
{
public:
	BackendResult start_pairing(const PairStartRequest&) override
	{
		m_snapshot.phase = "unsupported";
		m_snapshot.last_error = "real backend is not available on this platform";
		return {false, "real backend is not available on this platform"};
	}

	BackendResult enter_runtime() override
	{
		m_snapshot.phase = "unsupported";
		m_snapshot.last_error = "real backend is not available on this platform";
		return {false, "real backend is not available on this platform"};
	}

	BackendResult start_runtime(const PairStartRequest&) override
	{
		return enter_runtime();
	}

	BackendResult stop_session() override
	{
		m_snapshot = {};
		return {true, "session stopped"};
	}

	BackendSnapshot snapshot() const override
	{
		return m_snapshot;
	}

private:
	BackendSnapshot m_snapshot;
};
}

std::unique_ptr<SessionBackend> create_default_session_backend()
{
	return std::make_unique<NoopBackend>();
}
}
