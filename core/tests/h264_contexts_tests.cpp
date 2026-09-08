#include "drh/encoder/x264/contexts.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

uint64_t Hash(const barista::drh::x264::H264CabacContexts& contexts)
{
    uint64_t hash = 1469598103934665603ULL;
    for (const uint8_t state : contexts)
    {
        hash ^= state;
        hash *= 1099511628211ULL;
    }
    return hash;
}
}

int main()
{
    const auto intra = barista::drh::x264::InitializeGamepadCabacContexts(
        barista::drh::x264::H264SliceKind::Intra);
    const auto predicted = barista::drh::x264::InitializeGamepadCabacContexts(
        barista::drh::x264::H264SliceKind::Predicted);

    Check(Hash(intra) == 0xf42172c6a9f2cf56ULL,
        "QP 32 intra CABAC contexts differ from the H.264 oracle");
    Check(Hash(predicted) == 0x9d1eade74d8b0980ULL,
        "QP 32 predicted CABAC contexts differ from the H.264 oracle");
    Check(intra[3] == 50 && intra[60] == 82 && intra[459] == 65,
        "intra context landmarks changed");
    Check(predicted[11] == 97 && predicted[14] == 22 && predicted[459] == 3,
        "predicted context landmarks changed");

    std::cout << "GamePad CABAC context tests passed\n";
}
