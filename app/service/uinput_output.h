#pragma once
#include "api/controller.h"
#include "api/desktop_input.h"
#include <array>
#include <chrono>
#include <deque>
#include <string_view>
namespace barista {
class UinputOutput final : public ControllerOutput {
public:
    ~UinputOutput() override { Stop(); }
    bool Start(std::string& error) override;
    bool StartDesktop(std::string& error);
    bool Submit(const ControllerState& state) override;
    bool RumbleActive();
    bool TypeDesktopText(std::string_view text);
    void CancelDesktopText() { m_text.clear(); }
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
    bool StartDevice(std::string& error, bool desktop);
    bool SubmitDesktop(const ControllerState& state);
    std::deque<char> m_text;
    bool m_desktop = false;
    DesktopInputMapper m_mapper;
    std::chrono::steady_clock::time_point m_lastSubmit{};
    DesktopInput m_lastDesktop{};
    void ProcessForceFeedback();
    int m_fd = -1;
    std::array<RumbleEffect, 16> m_effects{};
};
}
