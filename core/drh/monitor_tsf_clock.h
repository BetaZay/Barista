#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

namespace barista::drh
{
// The caller synchronizes updates and reads. Some drivers (including rtw88)
// put only TSF's low 32 bits into radiotap's 64-bit field. Keep the kernel RX
// timestamp as the anchor, but compare those counters modulo 2^32.
class MonitorTsfClock
{
public:
    bool Update(uint64_t tsf, int64_t sampledAtUs)
    {
        if (sampledAtUs <= 0)
            return false;
        if (m_initialized)
        {
            if (sampledAtUs <= m_sampledAtUs)
                return false;
            if (tsf <= std::numeric_limits<uint32_t>::max() &&
                m_tsf <= std::numeric_limits<uint32_t>::max())
            {
                const uint32_t advance = static_cast<uint32_t>(tsf) -
                    static_cast<uint32_t>(m_tsf);
                // Serial ordering is ambiguous at half the counter range.
                // Reject duplicates and reordered samples on either side of
                // wrap; a zero counter itself is a valid sample.
                if (advance == 0 || advance >= 0x80000000u ||
                    sampledAtUs - m_sampledAtUs >= 0x80000000LL)
                    return false;
            }
            else if (tsf <= m_tsf)
                return false;
        }
        m_tsf = tsf;
        m_sampledAtUs = sampledAtUs;
        m_initialized = true;
        return true;
    }

    bool Initialized() const { return m_initialized; }

    std::optional<uint32_t> Timestamp(int64_t nowUs) const
    {
        if (!m_initialized)
            return {};
        const auto elapsed = static_cast<uint64_t>(std::max<int64_t>(0, nowUs - m_sampledAtUs));
        return static_cast<uint32_t>(m_tsf + elapsed);
    }

private:
    uint64_t m_tsf = 0;
    int64_t m_sampledAtUs = 0;
    bool m_initialized = false;
};
}
