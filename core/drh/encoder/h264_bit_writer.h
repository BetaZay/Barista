#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace barista::drh
{
class H264BitWriter
{
public:
    void WriteBit(bool value);
    void WriteBits(uint32_t value, unsigned count);
    void WriteUnsignedExpGolomb(uint32_t value);
    void WriteSignedExpGolomb(int32_t value);
    void WriteRbspTrailingBits();
    void AlignWithZeroBits();

    size_t BitsWritten() const;
    const std::vector<uint8_t>& Bytes() const;

private:
    std::vector<uint8_t> m_bytes;
    unsigned m_bitOffset = 0;
};

// Converts an RBSP into an EBSP by inserting the H.264 emulation-prevention
// byte after two zero bytes when the next byte is in the range 0x00..0x03.
std::vector<uint8_t> EscapeH264Rbsp(std::span<const uint8_t> rbsp);
}
