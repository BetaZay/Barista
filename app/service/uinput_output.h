#pragma once
#include "api/controller.h"
#include <array>
#include <chrono>
namespace barista {
class UinputOutput final : public ControllerOutput {
public:
    ~UinputOutput() override { Stop(); }
    bool Start(std::string& error) override;
    bool Submit(const ControllerState& state) override;
    bool RumbleActive();
    void Stop() override;
private:
    struct RumbleEffect {
        bool valid = false;
        bool playing = false;
        uint16_t strong = 0;
        uint16_t weak = 0;
        uint16_t lengthMs = 0;
        uint16_t delayMs = 0;
        std::chrono::steady_clock::time_point starts{};
        std::chrono::steady_clock::time_point ends{};
    };
    void ProcessForceFeedback();
    int m_fd = -1;
    std::array<RumbleEffect, 16> m_effects{};
};
}
