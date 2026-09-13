#include "uinput_output.h"
#include <linux/uinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>
namespace barista {
namespace {
constexpr std::pair<int,uint32_t> Buttons[] = {
    {BTN_SOUTH,0x8000}, {BTN_EAST,0x4000}, {BTN_NORTH,0x2000}, {BTN_WEST,0x1000},
    {BTN_TL,0x20}, {BTN_TR,0x10}, {BTN_START,0x8}, {BTN_SELECT,0x4}, {BTN_MODE,0x2},
    {BTN_THUMBL,0x800000}, {BTN_THUMBR,0x400000}
};
constexpr int Axes[] = {ABS_X,ABS_Y,ABS_RX,ABS_RY,ABS_Z,ABS_RZ,ABS_HAT0X,ABS_HAT0Y};
}
bool UinputOutput::Start(std::string& error)
{
    Stop();
    m_fd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    auto fail = [&] { error = std::string("Virtual controller: ") + strerror(errno); Stop(); return false; };
    if (m_fd < 0) return fail();
    if (ioctl(m_fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(m_fd, UI_SET_EVBIT, EV_ABS) < 0 ||
        ioctl(m_fd, UI_SET_EVBIT, EV_FF) < 0 || ioctl(m_fd, UI_SET_FFBIT, FF_RUMBLE) < 0)
        return fail();
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
    device.ff_effects_max = m_effects.size();
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
    add(EV_ABS, ABS_HAT0X, !!(state.buttons & 0x800) - !!(state.buttons & 0x400));
    add(EV_ABS, ABS_HAT0Y, !!(state.buttons & 0x100) - !!(state.buttons & 0x200));
    add(EV_SYN, SYN_REPORT, 0);
    const auto bytes = events.size() * sizeof(input_event);
    if (write(m_fd, events.data(), bytes) != static_cast<ssize_t>(bytes)) { Stop(); return false; }
    return true;
}

void UinputOutput::ProcessForceFeedback()
{
    if (m_fd < 0) return;
    std::array<input_event, 32> events{};
    while (true) {
        const ssize_t bytes = read(m_fd, events.data(), sizeof(events));
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
        if (bytes <= 0) return;
        const size_t count = static_cast<size_t>(bytes) / sizeof(input_event);
        for (size_t index = 0; index < count; ++index) {
            const input_event& event = events[index];
            if (event.type == EV_UINPUT && event.code == UI_FF_UPLOAD) {
                uinput_ff_upload upload{};
                upload.request_id = static_cast<uint32_t>(event.value);
                if (ioctl(m_fd, UI_BEGIN_FF_UPLOAD, &upload) < 0) continue;
                const int id = upload.effect.id;
                if (upload.effect.type != FF_RUMBLE || id < 0 ||
                    id >= static_cast<int>(m_effects.size())) {
                    upload.retval = -EINVAL;
                } else {
                    auto& effect = m_effects[static_cast<size_t>(id)];
                    effect.valid = true;
                    effect.strong = upload.effect.u.rumble.strong_magnitude;
                    effect.weak = upload.effect.u.rumble.weak_magnitude;
                    effect.lengthMs = upload.effect.replay.length;
                    effect.delayMs = upload.effect.replay.delay;
                    upload.retval = 0;
                }
                (void)ioctl(m_fd, UI_END_FF_UPLOAD, &upload);
            } else if (event.type == EV_UINPUT && event.code == UI_FF_ERASE) {
                uinput_ff_erase erase{};
                erase.request_id = static_cast<uint32_t>(event.value);
                if (ioctl(m_fd, UI_BEGIN_FF_ERASE, &erase) < 0) continue;
                if (erase.effect_id < m_effects.size()) {
                    m_effects[erase.effect_id] = {};
                    erase.retval = 0;
                } else {
                    erase.retval = -EINVAL;
                }
                (void)ioctl(m_fd, UI_END_FF_ERASE, &erase);
            } else if (event.type == EV_FF && event.code < m_effects.size()) {
                auto& effect = m_effects[event.code];
                effect.playing = effect.valid && event.value > 0;
                if (effect.playing) {
                    const auto now = std::chrono::steady_clock::now();
                    effect.starts = now + std::chrono::milliseconds(effect.delayMs);
                    effect.ends = effect.lengthMs == 0 ? std::chrono::steady_clock::time_point::max() :
                        effect.starts + std::chrono::milliseconds(
                            static_cast<uint64_t>(effect.lengthMs) * std::max(event.value, 1));
                }
            }
        }
    }
}

bool UinputOutput::RumbleActive()
{
    ProcessForceFeedback();
    const auto now = std::chrono::steady_clock::now();
    return std::any_of(m_effects.begin(), m_effects.end(), [now](RumbleEffect& effect) {
        if (effect.playing && now >= effect.ends) effect.playing = false;
        return effect.playing && now >= effect.starts && (effect.strong != 0 || effect.weak != 0);
    });
}

void UinputOutput::Stop()
{
    if (m_fd >= 0) { ioctl(m_fd, UI_DEV_DESTROY); close(m_fd); m_fd = -1; }
    m_effects = {};
}
}
