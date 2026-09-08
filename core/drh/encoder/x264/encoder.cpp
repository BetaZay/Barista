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

int ConfiguredQuantizer()
{
    const char* selected = std::getenv("DRCD_VIDEO_QP");
    if (selected)
    {
        if (std::strcmp(selected, "28") == 0)
            return 28;
        if (std::strcmp(selected, "32") == 0)
            return 32;
        if (std::strcmp(selected, "36") == 0)
            return 36;
        return 32;
    }

    const char* legacy = std::getenv("DRCD_QP28");
    return legacy && std::strcmp(legacy, "1") == 0 ? 28 : 32;
}

}

EncoderOptions OptionsFromEnvironment(bool preserveReplayFrameTypes)
{
    const char* fastSearch = std::getenv("DRCD_FAST_ENCODE");
    const char* legacyQuality = std::getenv("DRCD_LEGACY_ENCODER_QUALITY");
    return {
        .quantizer = ConfiguredQuantizer(),
        .fastSearch = fastSearch && std::strcmp(fastSearch, "1") == 0,
        .intraRefresh = DefaultEnabled("DRCD_INTRA_REFRESH"),
        .legacyQuality = legacyQuality && std::strcmp(legacyQuality, "1") == 0,
        .preserveReplayFrameTypes = preserveReplayFrameTypes,
        .disablePlanarPrediction = DefaultEnabled("DRCD_DISABLE_PLANAR_PREDICTION"),
    };
}

std::unique_ptr<VideoEncoder> CreateEncoder(const EncoderOptions& options)
{
    return std::make_unique<NativeEncoder>(options);
}
}
