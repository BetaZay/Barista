#include "drh/encoder/encoder.h"
#include "drh/encoder/x264/encoder.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void CheckChunks(const barista::drh::EncodedVideoFrame& frame)
{
    for (size_t index = 0; index < frame.chunks.size(); ++index)
    {
        const auto& chunk = frame.chunks[index];
        Check(!chunk.bytes.empty(), "encoder returned an empty DRH chunk");
        Check(chunk.firstMacroblock ==
            static_cast<int>(index * barista::drh::DrcVideoMacroblocksPerChunk),
            "DRH chunk starts at the wrong macroblock");
        Check(chunk.lastMacroblock ==
            static_cast<int>((index + 1) * barista::drh::DrcVideoMacroblocksPerChunk - 1),
            "DRH chunk ends at the wrong macroblock");
    }
}
}

int main()
{
    unsetenv("DRCD_FAST_ENCODE");
    unsetenv("DRCD_DISABLE_PLANAR_PREDICTION");
    auto configured = barista::drh::x264::OptionsFromEnvironment();
    Check(!configured.fastSearch && configured.disablePlanarPrediction,
        "native environment defaults are unsafe");
    setenv("DRCD_FAST_ENCODE", "1", 1);
    setenv("DRCD_DISABLE_PLANAR_PREDICTION", "0", 1);
    configured = barista::drh::x264::OptionsFromEnvironment();
    Check(configured.fastSearch && !configured.disablePlanarPrediction,
        "native environment switches were ignored");
    unsetenv("DRCD_FAST_ENCODE");
    unsetenv("DRCD_DISABLE_PLANAR_PREDICTION");
    barista::drh::x264::EncoderOptions options;
    auto encoder = barista::drh::x264::CreateEncoder(options);
    Check(encoder && encoder->IsValid(), "could not create x264 encoder backend");

    std::string error;
    std::vector<uint8_t> invalid;
    Check(!encoder->Encode(invalid, true, error), "encoder accepted an incomplete frame");
    Check(!error.empty(), "invalid frame did not report an error");

    std::vector<uint8_t> frame(barista::drh::DrcVideoFrameBytes, 16);
    std::fill(frame.begin() + barista::drh::DrcVideoWidth * barista::drh::DrcVideoHeight,
        frame.end(), 128);

    auto idr = encoder->Encode(frame, true, error);
    Check(idr.has_value() && idr->idr, "encoder did not produce a requested IDR");
    CheckChunks(*idr);

    auto predicted = encoder->Encode(frame, false, error);
    Check(predicted.has_value() && !predicted->idr, "encoder did not produce a predicted frame");
    CheckChunks(*predicted);

    std::cout << "x264 backend returned typed five-chunk DRH frames\n";
}
