#include "drh/encoder/h264_bit_writer.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
}

int main()
{
    barista::drh::H264BitWriter bits;
    bits.WriteBits(0b101, 3);
    bits.WriteUnsignedExpGolomb(0);
    bits.WriteUnsignedExpGolomb(1);
    bits.WriteUnsignedExpGolomb(2);
    bits.AlignWithZeroBits();
    Check(bits.BitsWritten() == 16, "bit writer reported the wrong length");
    Check(bits.Bytes() == std::vector<uint8_t>({0xb4, 0xc0}),
        "bit and unsigned Exp-Golomb output changed");

    barista::drh::H264BitWriter signedBits;
    signedBits.WriteSignedExpGolomb(0);
    signedBits.WriteSignedExpGolomb(1);
    signedBits.WriteSignedExpGolomb(-1);
    signedBits.WriteSignedExpGolomb(2);
    signedBits.WriteSignedExpGolomb(-2);
    signedBits.WriteRbspTrailingBits();
    Check(signedBits.Bytes() == std::vector<uint8_t>({0xa6, 0x42, 0xc0}),
        "signed Exp-Golomb or RBSP trailing output changed");

    const std::vector<uint8_t> rbsp{0, 0, 0, 1, 2, 3, 4, 0, 0, 3};
    const std::vector<uint8_t> expected{0, 0, 3, 0, 1, 2, 3, 4, 0, 0, 3, 3};
    Check(barista::drh::EscapeH264Rbsp(rbsp) == expected,
        "RBSP emulation-prevention output changed");

    bool rejected = false;
    try
    {
        bits.WriteBits(0, 33);
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    Check(rejected, "bit writer accepted a field wider than 32 bits");

    std::cout << "native H.264 bit writer tests passed\n";
}
