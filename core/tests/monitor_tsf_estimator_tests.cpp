#include "drh/monitor_tsf_estimator.h"

#include <cstdlib>
#include <iostream>

namespace
{
void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
}

int main()
{
    using barista::drh::MonitorTsfEstimator;
    constexpr int64_t host = 1000000;
    constexpr uint64_t radio = 5000000;
    MonitorTsfEstimator clock;
    Expect(!clock.Timestamp(host), "unsampled clock published a timestamp");
    Expect(clock.SampleAgeUs(host) == -1, "unsampled clock reported fresh data");
    Expect(clock.Update(radio, host), "initial sample rejected");
    // Bursty device delivery: hardware receipt precedes kernel receipt by
    // 5 ms. The good reference must survive these delayed samples.
    for (int64_t t = 10000; t <= 500000; t += 10000)
    {
        Expect(clock.Update(radio + t, host + t + 5000), "buffered sample rejected");
        Expect(clock.Timestamp(host + t + 5000) == radio + t + 5000,
            "buffering pulled the estimated radio clock backwards");
    }
    Expect(clock.CorrectionUs() == 5000, "receive-delay correction not reported");
    Expect(clock.SampleAgeUs(host + 600000) == 95000, "sample age is incorrect");
    Expect(clock.Timestamp(host + 700000) == radio + 700000,
        "missing samples stopped host-clock interpolation");

    // An initially buffered anchor improves once a low-latency packet arrives.
    clock = {};
    Expect(clock.Update(radio, host + 5000), "delayed initial sample rejected");
    Expect(clock.Update(radio + 10000, host + 10000), "better anchor rejected");
    Expect(clock.Timestamp(host + 20000) == radio + 20000, "initial buffering was not corrected");
    const auto rejected = clock.Rejected();
    Expect(!clock.Update(radio + 500000, host + 21000), "implausible forward jump accepted");
    Expect(clock.Rejected() == rejected + 1, "jump rejection not counted");
    Expect(clock.Update(radio + 30000, host + 30000), "bad sample poisoned subsequent updates");
    Expect(!clock.Update(radio + 30000, host + 31000), "duplicate accepted");
    Expect(!clock.Update(radio + 29000, host + 32000), "reordered counter accepted");
    Expect(!clock.Update(radio + 40000, host + 29000), "reordered host time accepted");

    // Window expiry follows oscillator drift instead of retaining an all-time
    // best offset. Exercise both directions with jitter for a simulated minute.
    for (const int drift : {-100, 100})
    {
        clock = {};
        Expect(clock.Update(radio, host), "drift fixture failed");
        uint32_t previous = static_cast<uint32_t>(radio);
        for (int64_t t = 10000; t <= 60000000; t += 10000)
        {
            const int64_t delay = (t / 10000) % 5 == 0 ? 0 : 4000;
            const int64_t hardware = radio + t + t * drift / 1000000;
            const auto before = clock.Timestamp(host + t + delay);
            Expect(clock.Update(hardware, host + t + delay), "drifting sample rejected");
            const auto estimate = *clock.Timestamp(host + t + delay);
            Expect(estimate >= *before && estimate >= previous, "clock stepped backwards on update");
            const int64_t actual = hardware + delay;
            Expect(std::abs(int64_t(estimate) - actual) < 200, "drift or jitter error exceeded 200 us");
            previous = estimate;
        }
    }

    clock = {};
    Expect(clock.Update(0xfffffff0ULL, host), "wrap fixture failed");
    Expect(clock.Update(0, host + 16), "zero-extended wrap rejected");
    Expect(clock.Timestamp(host + 32) == 16, "wire clock did not wrap");
    Expect(!clock.Update(0xfffffff8ULL, host + 33), "late pre-wrap packet accepted");
    Expect(clock.Update(32, host + 48), "clock did not recover after wrap");
    clock = {};
    Expect(clock.Update(0xfffffff0ULL, host) && clock.Update(0x100000020ULL, host + 48),
        "full-width wrap rejected");
    Expect(clock.Timestamp(host + 64) == 48, "full-width wrap changed the estimate");
    Expect(!clock.Update(0x200000030ULL, host + 64), "full-width clock jump hidden by wire truncation");
    // A persistent delay eventually replaces the old window. Corrections
    // remain bounded and closely spaced readers never see time go backwards.
    clock = {};
    Expect(clock.Update(radio, host), "expiry fixture failed");
    for (int64_t t = 10000; t <= 8000000; t += 10000)
    {
        const auto before = *clock.Timestamp(host + t + 5000);
        Expect(clock.Update(radio + t, host + t + 5000), "persistent-delay sample rejected");
        const auto after = *clock.Timestamp(host + t + 5000);
        Expect(after >= before, "window expiry published a backward step");
    }
    Expect(clock.CorrectionUs() == 0, "expired best sample was retained indefinitely");
    clock = {};
    Expect(!clock.Initialized() && clock.Accepted() == 0 && clock.Rejected() == 0,
        "session reset retained estimator history");
    Expect(clock.Update(0, host) && clock.Timestamp(host + 1) == 1, "initial zero counter failed");
    Expect(!clock.Update(1, host + 0x80000000LL), "ambiguous long gap accepted");
    std::cout << "Monitor TSF estimator tests passed\n";
}
