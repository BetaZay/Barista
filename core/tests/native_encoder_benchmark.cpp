#include "drh/encoder/x264/native_encoder.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>

int main(int argc, char** argv)
{
    using namespace barista::drh;
    x264::EncoderOptions options;
    options.fastSearch = argc > 1 && std::string(argv[1]) == "fast";
    x264::NativeEncoder encoder(options);
    std::vector<uint8_t> pixels(DrcVideoFrameBytes);
    std::vector<double> times;
    size_t bytes = 0;
    std::string error;
    for (unsigned frame = 0; frame < 600; ++frame)
    {
        for (unsigned y = 0; y < 480; ++y)
            for (unsigned x = 0; x < 864; ++x)
                pixels[y*864 + x] = 16 + ((x + y + frame*3 + ((x/13 ^ y/11) & 7)*17) % 220);
        for (unsigned y = 0; y < 240; ++y)
            for (unsigned x = 0; x < 432; ++x)
            {
                pixels[864*480 + y*432 + x] = 32 + ((x*3 + y + frame*2) % 192);
                pixels[864*480*5/4 + y*432 + x] = 32 + ((x + y*5 + frame*3) % 192);
            }
        const auto start = std::chrono::steady_clock::now();
        auto result = encoder.Encode(pixels, frame % 270 == 0, error);
        const auto end = std::chrono::steady_clock::now();
        if (!result)
        {
            std::cerr << error << '\n';
            return 1;
        }
        times.push_back(std::chrono::duration<double, std::milli>(end - start).count());
        for (const auto& chunk : result->chunks)
            bytes += chunk.bytes.size();
    }
    const double mean = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
    std::sort(times.begin(), times.end());
    constexpr double budget = 1000.0 / 59.94;
    std::cout << "600 moving frames; preset=" << (options.fastSearch ? "fast" : "default")
              << " mean_ms=" << mean << " p95_ms=" << times[569]
              << " p99_ms=" << times[593] << " max_ms=" << times.back()
              << " over_budget=" << std::count_if(times.begin(), times.end(),
                    [](double value) { return value > budget; })
              << " budget_ms=" << budget << " bytes=" << bytes << '\n';
    // Report timing, rather than making a machine-dependent CTest assertion.
}
