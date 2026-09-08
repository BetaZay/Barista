#pragma once

#include "api/media.h"

#include <optional>
#include <vector>

namespace barista::drh
{
struct EncodedVideoPacket
{
    uint64_t sourceSequence = 0;
    std::vector<uint8_t> bytes;
    bool keyFrame = false;
};

// Codec implementations live behind this portable contract. Packet scheduling
// belongs here, while radio transport stays in a platform backend.
class VideoEncoder
{
public:
    virtual ~VideoEncoder() = default;
    virtual std::optional<api::Error> Encode(
        const api::VideoFrame& frame,
        std::vector<EncodedVideoPacket>& packets) = 0;
};
}
