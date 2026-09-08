#include "drh/encoder/x264/bit_writer.h"

#include <bit>
#include <limits>
#include <stdexcept>

namespace barista::drh::x264
{
void H264BitWriter::WriteBit(bool value)
{
    if (m_bitOffset == 0)
        m_bytes.push_back(0);

    if (value)
        m_bytes.back() |= static_cast<uint8_t>(1U << (7U - m_bitOffset));

    m_bitOffset = (m_bitOffset + 1U) & 7U;
}

void H264BitWriter::WriteBits(uint32_t value, unsigned count)
{
    if (count > std::numeric_limits<uint32_t>::digits)
        throw std::invalid_argument("H.264 bit field exceeds 32 bits");

    for (unsigned index = count; index > 0; --index)
        WriteBit(((value >> (index - 1U)) & 1U) != 0);
}

void H264BitWriter::WriteUnsignedExpGolomb(uint32_t value)
{
    const uint64_t codeNumber = static_cast<uint64_t>(value) + 1U;
    const unsigned significantBits = std::bit_width(codeNumber);
    for (unsigned index = 1; index < significantBits; ++index)
        WriteBit(false);

    WriteBits(static_cast<uint32_t>(codeNumber), significantBits);
}

void H264BitWriter::WriteSignedExpGolomb(int32_t value)
{
    const int64_t wideValue = value;
    const uint64_t codeNumber = wideValue <= 0
        ? static_cast<uint64_t>(-wideValue) * 2U
        : static_cast<uint64_t>(wideValue) * 2U - 1U;
    if (codeNumber > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("signed Exp-Golomb value is out of range");

    WriteUnsignedExpGolomb(static_cast<uint32_t>(codeNumber));
}

void H264BitWriter::WriteRbspTrailingBits()
{
    WriteBit(true);
    AlignWithZeroBits();
}

void H264BitWriter::AlignWithZeroBits()
{
    while (m_bitOffset != 0)
        WriteBit(false);
}

size_t H264BitWriter::BitsWritten() const
{
    if (m_bytes.empty())
        return 0;
    return (m_bytes.size() - 1U) * 8U + (m_bitOffset == 0 ? 8U : m_bitOffset);
}

const std::vector<uint8_t>& H264BitWriter::Bytes() const
{
    return m_bytes;
}

std::vector<uint8_t> EscapeH264Rbsp(std::span<const uint8_t> rbsp)
{
    std::vector<uint8_t> escaped;
    escaped.reserve(rbsp.size() + rbsp.size() / 32U);

    unsigned zeroCount = 0;
    for (const uint8_t byte : rbsp)
    {
        if (zeroCount >= 2 && byte <= 3)
        {
            escaped.push_back(3);
            zeroCount = 0;
        }

        escaped.push_back(byte);
        zeroCount = byte == 0 ? zeroCount + 1U : 0U;
    }

    return escaped;
}
}
