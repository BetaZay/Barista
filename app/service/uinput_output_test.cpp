#include "uinput_output.h"
#include <linux/uinput.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
constexpr int DeviceFd = 12345;
std::vector<input_event> events;
bool created = false;

void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool HasEvent(int type, int code, int value)
{
    for (const auto& event : events)
        if (event.type == type && event.code == code && event.value == value)
            return true;
    return false;
}
}

// Intercept only this executable's uinput calls. Tests never open real devices
// or inject keyboard/mouse input into the desktop running the test suite.
extern "C" int __wrap_open(const char* path, int, ...)
{
    Check(std::strcmp(path, "/dev/uinput") == 0, "unexpected device open");
    return DeviceFd;
}
extern "C" int __wrap_ioctl(int fd, unsigned long request, ...)
{
    Check(fd == DeviceFd, "unexpected ioctl device");
    Check(request != EVIOCGRAB, "input output must never grab existing input");
    if (request == UI_DEV_CREATE)
        created = true;
    if (request == UI_DEV_DESTROY)
        created = false;
    return 0;
}
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t bytes)
{
    Check(fd == DeviceFd && created, "write outside device lifetime");
    Check(bytes % sizeof(input_event) == 0, "partial event");
    const auto* first = static_cast<const input_event*>(data);
    events.insert(events.end(), first, first + bytes / sizeof(input_event));
    return bytes;
}
extern "C" int __wrap_close(int fd)
{
    Check(fd == DeviceFd && !created, "device must be destroyed before close");
    return 0;
}

int main()
{
    barista::UinputOutput output;
    std::string error;
    Check(output.StartDesktop(error), "desktop device creation");
    Check(events.empty(), "neutral desktop must not emit input");
    barista::ControllerState state;
    state.buttons = 0x8000; // A.
    Check(output.Submit(state), "left press");
    Check(HasEvent(EV_KEY, BTN_LEFT, 1), "A must press left mouse");
    events.clear();
    Check(output.Submit(state) && events.empty(), "held button must not be pressed repeatedly");
    state.buttons = 0x40; // ZR holds the same mouse button after A releases.
    Check(output.Submit(state) && events.empty(), "shared button remains held");
    Check(output.Submit({}), "button release");
    Check(HasEvent(EV_KEY, BTN_LEFT, 0), "neutral state must release mouse");
    events.clear();
    Check(output.Submit({}) && events.empty(), "idle must leave desktop input alone");
    for (const auto buttons : {0x800u, 0x400u})
    {
        state.buttons = buttons;
        Check(output.Submit(state), "desktop D-pad press");
        const int key = buttons == 0x800u ? KEY_LEFT : KEY_RIGHT;
        Check(HasEvent(EV_KEY, key, 1), "desktop D-pad direction");
        events.clear();
        Check(output.Submit({}), "desktop D-pad release");
        Check(HasEvent(EV_KEY, key, 0), "neutral state must release arrow key");
        events.clear();
    }
    Check(!output.TypeDesktopText("\xc3\xa9"), "desktop typing accepted unsupported characters");
    Check(output.TypeDesktopText("aA! "), "queue desktop text");
    Check(!output.TypeDesktopText("b"), "pending typing overwritten");
    for (const auto [key, shifted] : {std::pair{KEY_A, false}, {KEY_A, true}, {KEY_1, true}, {KEY_SPACE, false}})
    {
        events.clear();
        Check(output.Submit({}), "type character");
        Check(HasEvent(EV_KEY, key, 1) && HasEvent(EV_KEY, key, 0), "typing must press and release");
        Check(HasEvent(EV_KEY, KEY_LEFTSHIFT, 1) == shifted, "typing shift mapping");
        if (shifted) Check(HasEvent(EV_KEY, KEY_LEFTSHIFT, 0), "typing left shift held");
    }
    events.clear();
    Check(output.Submit({}) && events.empty(), "typing must leave input neutral");
    Check(output.TypeDesktopText("cancel me"), "queue cancelled typing");
    output.CancelDesktopText();
    Check(output.Submit({}) && events.empty(), "cancelled typing emitted input");
    Check(output.Start(error), "controller device creation");
    Check(!output.TypeDesktopText("a"), "typing enabled in controller mode");
    for (const auto buttons : {0x800u, 0x400u, 0xc00u, 0u})
    {
        events.clear();
        state.buttons = buttons;
        Check(output.Submit(state), "controller D-pad");
        const int expected = buttons == 0x800u ? -1 : buttons == 0x400u ? 1 : 0;
        Check(HasEvent(EV_ABS, ABS_HAT0X, expected), "controller D-pad direction");
    }
    output.Stop();
    Check(!created, "device cleanup");
    std::cout << "uinput: idle, press/release, shared buttons and D-pad directions passed\n";
}
