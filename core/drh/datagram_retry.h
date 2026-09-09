#pragma once

#include <cerrno>
#include <chrono>
#include <thread>

namespace barista::drh
{
inline bool TemporarySendError(int error)
{
    return error == EAGAIN || error == EWOULDBLOCK || error == ENOBUFS || error == EINTR;
}

// Attempt returns zero on success or a captured errno on failure. Never resend
// a successfully accepted datagram. A shared deadline bounds the whole frame.
template<class Attempt, class Now, class Pause>
int RetryDatagram(Attempt attempt, std::chrono::steady_clock::time_point deadline,
    Now now, Pause pause)
{
    for (;;)
    {
        if (now() >= deadline) return EAGAIN;
        const int error = attempt();
        if (!error || !TemporarySendError(error)) return error;
        if (now() >= deadline) return error;
        pause();
    }
}
}
