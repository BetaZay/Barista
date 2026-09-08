#pragma once
#include <array>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace barista::drh {
inline constexpr auto kFormatVideoLead = std::chrono::microseconds(5000);
inline constexpr uint32_t kFormatTimestampAgeUs = 1250;
// Format is fresh when sent, then video starts with the same timestamp aged
// 6250us. Rebase before publishing either packet, never change a published stamp.
inline std::chrono::steady_clock::time_point NextFormatDeadline(
    std::chrono::steady_clock::time_point previous_video,
    std::chrono::steady_clock::time_point now, bool early_format)
{
    return std::max(previous_video + std::chrono::microseconds(16683) -
        (early_format ? kFormatVideoLead : std::chrono::microseconds(0)), now);
}
inline constexpr uint32_t FormatVideoTimestamp(uint32_t ap_tsf)
{
    return ap_tsf - kFormatTimestampAgeUs;
}
// Keep single-packet chunks at their original start deadline. Spread multi-packet
// chunks in both IDR and P frames through the existing IDR completion windows.
inline std::chrono::microseconds VideoPacketOffset(size_t chunk,
    size_t packet, size_t count)
{
    constexpr std::array<int64_t, 5> starts{0, 3000, 6000, 9000, 11000};
    constexpr std::array<int64_t, 5> ends{2500, 5000, 7500, 10000, 13000};
    if (chunk >= 5 || count == 0 || packet >= count)
        throw std::invalid_argument("invalid video packet schedule");
    const auto extra = count > 1 ?
        (ends[chunk] - starts[chunk]) * static_cast<int64_t>(packet) / static_cast<int64_t>(count-1) : 0;
    return std::chrono::microseconds(starts[chunk] + extra);
}
}
