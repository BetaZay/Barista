#include "drh/encoder/x264/native_encoder.h"

#include <iostream>

int main()
{
    using namespace barista::drh;
    x264::NativeEncoder encoder(x264::OptionsFromEnvironment());
    std::vector<uint8_t> pixels(DrcVideoFrameBytes);
    std::string error;
    char idr;
    while (std::cin.get(idr))
    {
        if (!std::cin.read(reinterpret_cast<char*>(pixels.data()), pixels.size()))
            return 1;
        auto frame = encoder.Encode(pixels, idr != 0, error);
        if (!frame)
        {
            std::cerr << error << '\n';
            return 1;
        }
        uint32_t size = 0;
        for (const auto& chunk : frame->chunks)
            size += chunk.bytes.size();
        for (unsigned shift = 0; shift < 32; shift += 8)
            std::cout.put(static_cast<char>(size >> shift));
        for (const auto& chunk : frame->chunks)
            std::cout.write(reinterpret_cast<const char*>(chunk.bytes.data()), chunk.bytes.size());
        const auto reference = encoder.ReferencePicture();
        for (unsigned plane = 0; plane < 3; ++plane)
            for (unsigned row = 0; row < (plane ? 240u : 480u); ++row)
                std::cout.write(reinterpret_cast<const char*>(reference.yuv[plane] +
                    row * reference.stride[plane]), plane ? 432 : 864);
        if (!std::cout)
            return 1;
    }
    return std::cin.eof() ? 0 : 1;
}
