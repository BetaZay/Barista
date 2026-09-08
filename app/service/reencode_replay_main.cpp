#include "drh/encoder/media_streamer.h"
#include <iostream>
#include <string>

int main()
{
    std::string error;
    if (!barista::drh::MediaStreamer::reencode_replay(std::cin, std::cout, error))
    {
        std::cerr << error << '\n';
        return 1;
    }
    return 0;
}
