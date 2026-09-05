#include "drcd/media_streamer.h"
#include <iostream>
#include <string>

int main()
{
    std::string error;
    if (!drcd::MediaStreamer::reencode_replay(std::cin, std::cout, error))
    {
        std::cerr << error << '\n';
        return 1;
    }
    return 0;
}
