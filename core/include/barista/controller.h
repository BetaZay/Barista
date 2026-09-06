#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace barista {
// Native reports stop here. Platform output drivers consume normalized state.
struct ControllerState {
    uint32_t buttons = 0;
    std::array<int, 4> sticks{}; // X/Y, RX/RY; -32767..32767, positive Y is down
};
inline ControllerState DecodeInput(std::span<const uint8_t> report)
{
    ControllerState state;
    if (report.size() != 128) return state;
    state.buttons = uint32_t(report[80]) << 16 | uint32_t(report[2]) << 8 | report[3];
    for (size_t axis = 0; axis < 4; ++axis) {
        const size_t offset = 6 + axis * 2;
        int value = int(report[offset]) | int(report[offset + 1]) << 8;
        value = std::clamp(value - 2050, -1150, 1150);
        if (value > -115 && value < 115) value = 0;
        state.sticks[axis] = value * 32767 / 1150 * (axis % 2 ? -1 : 1);
    }
    return state;
}
class ControllerOutput {
public:
    virtual ~ControllerOutput() = default;
    virtual bool Start(std::string& error) = 0;
    virtual bool Submit(const ControllerState& state) = 0;
    virtual void Stop() = 0;
};
inline bool ValidInterface(const std::string& name)
{
    if (name.empty() || name.size() > 15 || name.front() == '-' || name == "." || name == "..") return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}
inline bool ValidPairCode(const std::string& code)
{
    return code.size() == 4 && std::all_of(code.begin(), code.end(), [](char c) { return c >= '0' && c <= '3'; });
}
}
