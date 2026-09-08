#pragma once

#include "drh/encoder/encoder.h"

#include <memory>

namespace barista::drh::x264
{
struct EncoderOptions
{
    bool fastSearch = false;
    bool disablePlanarPrediction = true;
};

EncoderOptions OptionsFromEnvironment();
std::unique_ptr<VideoEncoder> CreateEncoder(const EncoderOptions& options);
}
