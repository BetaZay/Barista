#include "barista/controller.h"
#include <iostream>
#include <stdexcept>
int main()
{
    auto check = [](bool value) { if (!value) throw std::runtime_error("core regression"); };
    check(barista::ValidInterface("wlan0"));
    for (auto value : {"", "../wlan0", "-x", "a b", "a\nb", "abcdefghijklmnop", ".", ".."})
        check(!barista::ValidInterface(value));
    check(barista::ValidPairCode("0123"));
    for (auto value : {"", "123", "4321", "0123\n", "-123"}) check(!barista::ValidPairCode(value));
    std::array<uint8_t,128> raw{};
    for (size_t offset : {6,8,10,12}) { raw[offset] = 2; raw[offset+1] = 8; }
    check(barista::DecodeInput(raw).sticks == std::array<int,4>{});
    raw[2] = 0x80; raw[80] = 0x40;
    check(barista::DecodeInput(raw).buttons == 0x408000);
    raw[6] = 0; raw[7] = 0; raw[8] = 0xff; raw[9] = 0xff;
    check(barista::DecodeInput(raw).sticks[0] == -32767);
    check(barista::DecodeInput(raw).sticks[1] == -32767);
    check(barista::DecodeInput(std::span(raw).first(80)).buttons == 0);
    std::cout << "Input normalization and privileged argument validation passed\n";
}
