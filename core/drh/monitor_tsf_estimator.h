#pragma once

#include "monitor_tsf_clock.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <optional>

namespace barista::drh
{
// Experimental receive-delay filter. For a stable clock, TSF - host receipt
// time is largest for the least-delayed packet. Keep the best sample in each
// 100-ms bucket, then select the best of the last second. This removes variable
// buffering delay, not a constant hardware/driver offset. The host clock drives
// interpolation; expiring buckets allow hardware drift to be followed.
// The caller synchronizes updates and reads, as with MonitorTsfClock.
class MonitorTsfEstimator
{
public:
    bool Update(uint64_t tsf, int64_t sampledAtUs)
    {
        auto candidate = m_raw;
        if (!candidate.Update(tsf, sampledAtUs))
        {
            ++m_rejected;
            return false;
        }
        const bool first = !Initialized();
        const auto elapsed = first ? 0 : sampledAtUs - m_lastSampleUs;
        const uint64_t advance = tsf <= UINT32_MAX && m_lastTsf <= UINT32_MAX
            ? static_cast<uint32_t>(tsf) - static_cast<uint32_t>(m_lastTsf)
            : tsf - m_lastTsf;
        // A large forward jump is not explained by ordinary oscillator drift
        // or a small change in receive latency. Do not poison the reference.
        if (!first && (elapsed >= 0x80000000LL || advance >= 0x80000000u ||
            int64_t(advance) - elapsed > 10000))
        {
            ++m_rejected;
            return false;
        }
        m_raw = candidate;
        if (first)
        {
            m_originUs = sampledAtUs;
            m_originTsf = static_cast<uint32_t>(tsf);
        }
        else
            m_elapsedTsf += advance;
        m_lastSampleUs = sampledAtUs;
        m_lastTsf = tsf;
        ++m_accepted;

        const int64_t bucketId = sampledAtUs / 100000;
        auto& bucket = m_buckets[bucketId % m_buckets.size()];
        const int64_t offset = m_elapsedTsf - (sampledAtUs - m_originUs);
        if (bucket.id != bucketId)
            bucket = {bucketId, offset};
        else
            bucket.offset = std::max(bucket.offset, offset);
        int64_t best = offset;
        for (const auto& entry : m_buckets)
        {
            if (entry.id >= 0 && bucketId - entry.id < int64_t(m_buckets.size()))
                best = std::max(best, entry.offset);
        }
        // Better samples can immediately remove previously observed delay.
        // Downward correction is capped at 1000 ppm of elapsed host time.
        if (best >= m_offsetUs)
            m_offsetUs = static_cast<double>(best);
        else
            m_offsetUs = std::max(static_cast<double>(best), m_offsetUs - elapsed * 0.001);
        return true;
    }

    bool Initialized() const { return m_raw.Initialized(); }

    std::optional<uint32_t> Timestamp(int64_t nowUs) const
    {
        if (!Initialized())
            return {};
        const int64_t elapsed = std::max<int64_t>(0, nowUs - m_originUs);
        const int64_t timestamp = int64_t(m_originTsf) + elapsed +
            static_cast<int64_t>(m_offsetUs);
        // Updates may arrive between two closely spaced reads, or carry an
        // older kernel receipt time. Never publish a backward step (apart
        // from wire wrap). A downward correction can briefly hold time still.
        m_lastPublished = std::max(m_lastPublished, timestamp);
        return static_cast<uint32_t>(m_lastPublished);
    }

    int64_t SampleAgeUs(int64_t nowUs) const
    {
        return Initialized() ? std::max<int64_t>(0, nowUs - m_lastSampleUs) : -1;
    }
    int64_t CorrectionUs() const
    {
        return static_cast<int64_t>(m_offsetUs) -
            (m_elapsedTsf - (m_lastSampleUs - m_originUs));
    }
    uint64_t Accepted() const { return m_accepted; }
    uint64_t Rejected() const { return m_rejected; }

private:
    struct Bucket
    {
        int64_t id = -1;
        int64_t offset = 0;
    };
    std::array<Bucket, 10> m_buckets{};
    MonitorTsfClock m_raw;
    int64_t m_originUs = 0;
    uint32_t m_originTsf = 0;
    int64_t m_lastSampleUs = 0;
    uint64_t m_lastTsf = 0;
    int64_t m_elapsedTsf = 0;
    double m_offsetUs = 0;
    uint64_t m_accepted = 0;
    uint64_t m_rejected = 0;
    mutable int64_t m_lastPublished = 0;
};
}
