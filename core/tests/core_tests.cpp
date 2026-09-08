#include "api/controller.h"
#include "api/media.h"
#include "drh/encoder/encoder.h"
#include "drh/server/session_server.h"
#include <iostream>
#include <stdexcept>
int main()
{
    auto check = [](bool value) { if (!value) throw std::runtime_error("core regression"); };
    check(barista::api::ValidInterfaceName("wlan0"));
    check(barista::api::ValidInterfaceName("wlan1"));
    for (auto value : {"", "../wlan0", "-x", "a b", "a\nb", "abcdefghijklmnop", ".", ".."})
        check(!barista::api::ValidInterfaceName(value));
    check(barista::api::ParsePairCode("0123").has_value());
    check(barista::api::PairCodeName(*barista::api::ParsePairCode("0123")) == "0123");
    for (auto value : {"", "123", "4321", "0123\n", "-123"}) check(!barista::api::ParsePairCode(value));
    std::array<uint8_t,128> raw{};
    for (size_t offset : {6,8,10,12}) { raw[offset] = 2; raw[offset+1] = 8; }
    check(barista::DecodeInput(raw).sticks == std::array<int,4>{});
    raw[2] = 0x80; raw[80] = 0x40;
    check(barista::DecodeInput(raw).buttons == 0x408000);
    raw[6] = 0; raw[7] = 0; raw[8] = 0xff; raw[9] = 0xff;
    check(barista::DecodeInput(raw).sticks[0] == -32767);
    check(barista::DecodeInput(raw).sticks[1] == -32767);
    check(barista::DecodeInput(std::span(raw).first(80)).buttons == 0);
    const barista::api::SessionStatus status;
    check(status.apiVersion == barista::api::ApiVersion);
    check(barista::api::ParseSessionMode("real") == barista::api::SessionMode::Real);
    check(barista::api::ParseSessionMode("controller") == barista::api::SessionMode::Controller);
    check(!barista::api::ParseSessionMode("invalid"));
    check(barista::api::SessionModeName(barista::api::SessionMode::Controller) == "controller");
    check(barista::api::ParseSessionPhase("runtime") == barista::api::SessionPhase::Runtime);
    check(!barista::api::ParseSessionPhase("unknown"));
    check(barista::api::SessionPhaseName(barista::api::SessionPhase::Stopping) == "stopping");
    const barista::api::VideoFrame frame;
    check(frame.i420.empty());
    std::cout << "Input normalization and privileged argument validation passed\n";
}
