#pragma once
#include "controller.h"
#include <cmath>

namespace barista
{
struct DesktopInput
{
    int x = 0, y = 0, wheel = 0, horizontalWheel = 0;
    bool left = false, right = false, middle = false;
    bool enter = false, escape = false, tab = false, backspace = false;
    bool up = false, down = false, previous = false, next = false;
};

// State-based mapping: callers submit neutral state when input goes stale.
// Fractional accumulation makes cursor speed independent of polling frequency.
class DesktopInputMapper
{
  public:
    DesktopInput Map(const ControllerState &state, double seconds)
    {
        seconds = std::clamp(seconds, 0.0, 0.05);
        auto move = [seconds](int axis, double speed, double &remainder)
        {
            double normalized = std::clamp(axis / 32767.0, -1.0, 1.0);
            constexpr double deadzone = 0.15;
            if (std::abs(normalized) <= deadzone)
            {
                remainder = 0;
                return 0;
            }
            normalized =
                std::copysign((std::abs(normalized) - deadzone) / (1 - deadzone), normalized);
            remainder += normalized * std::abs(normalized) * speed * seconds;
            const int delta = static_cast<int>(remainder);
            remainder -= delta;
            return delta;
        };
        const auto held = [&](uint32_t mask) { return bool(state.buttons & mask); };
        DesktopInput result;
        result.x = move(state.sticks[0], 1000, m_x);
        result.y = move(state.sticks[1], 1000, m_y);
        result.wheel = -move(state.sticks[3], 12, m_wheel);
        result.horizontalWheel = move(state.sticks[2], 12, m_horizontalWheel);
        result.left = held(0x8000) || held(0x40);  // A / ZR
        result.right = held(0x4000) || held(0x80); // B / ZL
        result.middle = held(0x800000);            // Left stick click
        result.enter = held(0x8);                  // Plus
        result.escape = held(0x4);                 // Minus
        result.tab = held(0x1000);                 // Y
        result.backspace = held(0x2000);           // X
        result.up = held(0x200);
        result.down = held(0x100);
        result.previous = held(0x800); // D-pad left
        result.next = held(0x400);     // D-pad right
        return result;
    }
    void Reset()
    {
        m_x = m_y = m_wheel = m_horizontalWheel = 0;
    }

  private:
    double m_x = 0, m_y = 0, m_wheel = 0, m_horizontalWheel = 0;
};
} // namespace barista
