#include "drh/datagram_retry.h"
#include "drh/encoder/video_send_recovery.h"
#include <array>
#include <iostream>

int main()
{
    using namespace std::chrono;
    using barista::drh::RetryDatagram;
    steady_clock::time_point now{};
    const auto clock = [&] { return now; };
    const auto pause = [&] { now += microseconds(100); };
    int calls = 0;
    const std::array errors{EAGAIN, ENOBUFS, EINTR, 0};
    if (RetryDatagram([&] { return errors.at(calls++); }, now + milliseconds(2), clock, pause)) return 1;
    if (calls != 4 || now.time_since_epoch() != microseconds(300)) return 1;
    calls = 0;
    const auto deadline = now + microseconds(300);
    if (RetryDatagram([&] { ++calls; return EAGAIN; }, deadline, clock, pause) != EAGAIN) return 1;
    if (calls != 3 || now != deadline) return 1;
    calls = 0;
    if (RetryDatagram([&] { ++calls; return EIO; }, now + milliseconds(2), clock, pause) != EIO) return 1;
    if (calls != 1) return 1;
    calls = 0;
    if (RetryDatagram([&] { ++calls; return 0; }, now, clock, pause) != EAGAIN || calls) return 1;
    // A later frame gets a new deadline and can send successfully after timeout.
    if (RetryDatagram([&] { ++calls; return 0; }, now + milliseconds(2), clock, pause) || calls != 1) return 1;
    std::cout << "Transient retries, deadline exhaustion, fatal errors and resumed sends passed\n";
    barista::drh::VideoSendRecovery recovery;
    if (!recovery.CanSend(false) || recovery.ConsumeRequest()) return 1;
    recovery.Failed();
    if (!recovery.ConsumeRequest() || recovery.ConsumeRequest()) return 1;
    // P already encoded in parallel must be skipped and renew the IDR request.
    if (recovery.CanSend(false) || !recovery.ConsumeRequest()) return 1;
    if (!recovery.CanSend(true)) return 1;
    recovery.Failed();
    if (recovery.CanSend(false) || !recovery.ConsumeRequest()) return 1;
    recovery.Delivered(true);
    if (!recovery.CanSend(false) || recovery.ConsumeRequest()) return 1;
    std::cout << "Failed-frame recovery gates dependent P frames until delivered IDR\n";
}
