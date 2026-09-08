#pragma once

#include "drh/encoder/encoder.h"

#include <memory>

namespace barista::drh::x264
{
struct EncoderOptions
{
    int quantizer = 32;
    bool fastSearch = false;
    bool intraRefresh = true;
    bool legacyQuality = false;
    bool preserveReplayFrameTypes = false;
    bool disablePlanarPrediction = true;
};

EncoderOptions OptionsFromEnvironment(bool preserveReplayFrameTypes = false);
std::unique_ptr<VideoEncoder> CreateEncoder(const EncoderOptions& options);
}
