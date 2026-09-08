#include "drh/encoder/x264/encoder.h"
#include "drh/encoder/x264/native_encoder.h"

#include <cstdlib>
#include <cstring>


namespace barista::drh::x264
{
namespace
{
bool DefaultEnabled(const char* name)
{
    const char* value = std::getenv(name);
    return !value || std::strcmp(value, "0") != 0;
}

}

EncoderOptions OptionsFromEnvironment()
{
    const char* fastSearch = std::getenv("DRCD_FAST_ENCODE");
    return {
        .fastSearch = fastSearch && std::strcmp(fastSearch, "1") == 0,
        .disablePlanarPrediction = DefaultEnabled("DRCD_DISABLE_PLANAR_PREDICTION"),
    };
}

std::unique_ptr<VideoEncoder> CreateEncoder(const EncoderOptions& options)
{
    return std::make_unique<NativeEncoder>(options);
}
}
