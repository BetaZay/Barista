#include "drh/server/interrupt.h"

#include <atomic>

namespace barista::drh
{
namespace
{
std::atomic_bool s_stop_requested = false;
}

void set_stop_requested(bool requested)
{
	s_stop_requested.store(requested);
}

bool is_stop_requested()
{
	return s_stop_requested.load();
}
}
