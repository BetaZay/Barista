#pragma once

#include "types.h"

#include <optional>

namespace barista::api
{
// Implementations must retain at most one not-yet-encoded video frame. A newer
// frame replaces it, preventing transport congestion from becoming latency.
class MediaProducer
{
public:
    virtual ~MediaProducer() = default;
    virtual std::optional<Error> SubmitVideo(VideoFrame frame) = 0;
    virtual std::optional<Error> SubmitAudio(AudioFrame frame) = 0;
};

class InputConsumer
{
public:
    virtual ~InputConsumer() = default;
    virtual std::optional<InputReport> ReadInput() = 0;
};
}
