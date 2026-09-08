#include "drh/encoder/h264_cabac.h"

#include <array>
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
    barista::drh::H264CabacWriter cabac(0xa5);
    std::array<barista::drh::H264CabacState, 3> states{64, 17, 126};
    for (int index = 0; index < 80; ++index)
    {
        cabac.EncodeDecision(states[index % 3], ((index * 7 + 3) & 1) != 0);
        if ((index % 5) == 0)
            cabac.EncodeBypass((index & 1) != 0);
        if ((index % 13) == 0)
            cabac.EncodeTerminal();
    }

    const std::vector<uint8_t> oracle{
        0xf2, 0xe6, 0xf8, 0xee, 0x61, 0x60, 0x36,
        0x0f, 0x6d, 0x88, 0xce, 0xac, 0x48, 0x6a,
    };
    Check(std::vector<uint8_t>(cabac.Payload().begin(), cabac.Payload().end()) == oracle,
        "native CABAC bytes differ from the legacy oracle");
    Check(states == std::array<barista::drh::H264CabacState, 3>{118, 119, 126},
        "native CABAC context transitions differ from the legacy oracle");
    Check(cabac.BytePosition() == oracle.size(), "CABAC position is inaccurate");
    Check(cabac.PrecedingByte() == 0xa5, "CABAC carried before its payload");

    cabac.Finish(7);
    Check(cabac.IsFinished(), "CABAC stream did not finalize");
    Check(cabac.BytePosition() == cabac.Payload().size(),
        "final CABAC position includes unresolved bytes");
    Check(cabac.Payload().size() > oracle.size(), "CABAC flush emitted no bytes");

    bool rejected = false;
    try
    {
        cabac.EncodeBypass(false);
    }
    catch (const std::logic_error&)
    {
        rejected = true;
    }
    Check(rejected, "CABAC writer accepted data after finalization");

    std::cout << "native H.264 CABAC parity tests passed\n";
}
