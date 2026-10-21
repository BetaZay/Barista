#pragma once

#include <cstdint>
#include <optional>

namespace barista::drh
{
// SO_TIMESTAMPNS records kernel receipt before the monitor socket is drained.
// Convert that timestamp into the steady-clock domain used for extrapolation.
// Reject missing/future/old samples instead of changing the TSF epoch.
inline std::optional<int64_t> MonitorTsfSampleTime(int64_t steadyUs,
    int64_t realtimeNs, int64_t receivedNs)
{
    if (realtimeNs <= 0 || receivedNs <= 0 || receivedNs > realtimeNs)
        return {};
    const int64_t ageNs = realtimeNs - receivedNs;
    if (ageNs > 1000000000 || steadyUs < ageNs / 1000)
        return {};
    return steadyUs - ageNs / 1000;
}
}
