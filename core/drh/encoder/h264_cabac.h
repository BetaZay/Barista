#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace barista::drh
{
// H.264 context state encoded as (pStateIdx << 1) | valMPS.
using H264CabacState = uint8_t;

class H264CabacWriter
{
public:
    explicit H264CabacWriter(uint8_t precedingByte = 0);

    void EncodeDecision(H264CabacState& state, bool bin);
    void EncodeBypass(bool bin);
    void EncodeTerminal();
    void Finish(unsigned frameIndex);

    size_t BytePosition() const;
    uint8_t PrecedingByte() const;
    std::span<const uint8_t> Payload() const;
    bool IsFinished() const;

private:
    void Renormalize();
    void PutByte();

    uint32_t m_low = 0;
    uint32_t m_range = 0x01fe;
    int m_queue = -9;
    size_t m_outstandingBytes = 0;
    std::vector<uint8_t> m_buffer;
    bool m_finished = false;
};
}
