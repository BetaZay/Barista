#include "uinput_output.h"
#include <linux/uinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <vector>
namespace barista {
namespace {
constexpr std::pair<int,uint32_t> Buttons[] = {
    {BTN_EAST,0x8000}, {BTN_SOUTH,0x4000}, {BTN_NORTH,0x2000}, {BTN_WEST,0x1000},
    {BTN_TL,0x20}, {BTN_TR,0x10}, {BTN_START,0x4}, {BTN_SELECT,0x8}, {BTN_MODE,0x2},
    {BTN_THUMBL,0x800000}, {BTN_THUMBR,0x400000}
};
constexpr int Axes[] = {ABS_X,ABS_Y,ABS_RX,ABS_RY,ABS_Z,ABS_RZ,ABS_HAT0X,ABS_HAT0Y};
}
bool UinputOutput::Start(std::string& error)
{
    Stop();
    m_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    auto fail = [&] { error = std::string("Virtual controller: ") + strerror(errno); Stop(); return false; };
    if (m_fd < 0) return fail();
    if (ioctl(m_fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(m_fd, UI_SET_EVBIT, EV_ABS) < 0) return fail();
    for (auto [button, mask] : Buttons) if (ioctl(m_fd, UI_SET_KEYBIT, button) < 0) return fail();
    for (auto axis : Axes) {
        if (ioctl(m_fd, UI_SET_ABSBIT, axis) < 0) return fail();
        uinput_abs_setup setup{}; setup.code = axis;
        setup.absinfo.minimum = axis == ABS_Z || axis == ABS_RZ ? 0 : axis >= ABS_HAT0X ? -1 : -32767;
        setup.absinfo.maximum = axis == ABS_Z || axis == ABS_RZ ? 255 : axis >= ABS_HAT0X ? 1 : 32767;
        if (ioctl(m_fd, UI_ABS_SETUP, &setup) < 0) return fail();
    }
    uinput_setup device{};
    std::strcpy(device.name, "Barista Wii U GamePad");
    device.id.bustype = BUS_VIRTUAL; device.id.vendor = 0x057e; device.id.product = 0; device.id.version = 1;
    if (ioctl(m_fd, UI_DEV_SETUP, &device) < 0 || ioctl(m_fd, UI_DEV_CREATE) < 0) return fail();
    return Submit({});
}
bool UinputOutput::Submit(const ControllerState& state)
{
    if (m_fd < 0) return false;
    std::vector<input_event> events;
    auto add = [&](int type, int code, int value) {
        input_event event{}; event.type = type; event.code = code; event.value = value; events.push_back(event);
    };
    for (auto [button, mask] : Buttons) add(EV_KEY, button, !!(state.buttons & mask));
    for (size_t i = 0; i < 4; ++i) add(EV_ABS, Axes[i], state.sticks[i]);
    add(EV_ABS, ABS_Z, state.buttons & 0x80 ? 255 : 0);
    add(EV_ABS, ABS_RZ, state.buttons & 0x40 ? 255 : 0);
    add(EV_ABS, ABS_HAT0X, !!(state.buttons & 0x100) - !!(state.buttons & 0x200));
    add(EV_ABS, ABS_HAT0Y, !!(state.buttons & 0x400) - !!(state.buttons & 0x800));
    add(EV_SYN, SYN_REPORT, 0);
    const auto bytes = events.size() * sizeof(input_event);
    if (write(m_fd, events.data(), bytes) != static_cast<ssize_t>(bytes)) { Stop(); return false; }
    return true;
}
void UinputOutput::Stop()
{
    if (m_fd >= 0) { ioctl(m_fd, UI_DEV_DESTROY); close(m_fd); m_fd = -1; }
}
}
