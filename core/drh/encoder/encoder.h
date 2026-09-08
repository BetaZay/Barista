#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace barista::drh
{
inline constexpr size_t DrcVideoWidth = 864;
inline constexpr size_t DrcVideoHeight = 480;
inline constexpr size_t DrcVideoFrameBytes = DrcVideoWidth * DrcVideoHeight * 3 / 2;
inline constexpr size_t DrcVideoChunkCount = 5;
inline constexpr size_t DrcVideoMacroblocksPerRow = DrcVideoWidth / 16;
inline constexpr size_t DrcVideoMacroblocksPerChunk = DrcVideoMacroblocksPerRow * 6;

struct EncodedVideoChunk
{
    std::vector<uint8_t> bytes;
    int nalType = 0;
    int referencePriority = 0;
    int firstMacroblock = 0;
    int lastMacroblock = 0;
};

struct EncodedVideoFrame
{
    bool idr = false;
    std::array<EncodedVideoChunk, DrcVideoChunkCount> chunks;
};

// Produces the five logical chunks expected by the GamePad. Codec-specific
// configuration stays behind this boundary; VSTRM packet construction and
// scheduling remain owned by MediaStreamer.
class VideoEncoder
{
public:
    virtual ~VideoEncoder() = default;
    virtual bool IsValid() const = 0;
    virtual std::optional<EncodedVideoFrame> Encode(
        std::span<uint8_t> i420,
        bool requestIdr,
        std::string& error) = 0;
};
}
