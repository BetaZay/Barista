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
constexpr char TextPlain[] = "1234567890-=qwertyuiop[]asdfghjkl;'zxcvbnm,./`\\ ";
constexpr char TextShifted[] = "!@#$%^&*()_+QWERTYUIOP{}ASDFGHJKL:\"ZXCVBNM<>?~| ";
constexpr int TextCodes[] = {
    KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
    KEY_MINUS, KEY_EQUAL, KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P,
    KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L,
    KEY_SEMICOLON, KEY_APOSTROPHE, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M,
    KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_GRAVE, KEY_BACKSLASH, KEY_SPACE
};
static_assert(sizeof(TextPlain) == sizeof(TextShifted));
static_assert(sizeof(TextPlain) - 1 == std::size(TextCodes));
constexpr int Axes[] = {ABS_X,ABS_Y,ABS_RX,ABS_RY,ABS_Z,ABS_RZ,ABS_HAT0X,ABS_HAT0Y};
}
bool UinputOutput::Start(std::string& error) { return StartDevice(error, false); }
bool UinputOutput::StartDesktop(std::string& error) { return StartDevice(error, true); }
bool UinputOutput::StartDevice(std::string& error, bool desktop)
{
    Stop();
    m_desktop = desktop;
    m_lastSubmit = std::chrono::steady_clock::now();
    m_fd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    auto fail = [&] { error = std::string("Virtual controller: ") + strerror(errno); Stop(); return false; };
    if (m_fd < 0) return fail();
    if (desktop) {
        if (ioctl(m_fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(m_fd, UI_SET_EVBIT, EV_REL) < 0) return fail();
        for (int key : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, KEY_ENTER, KEY_ESC, KEY_TAB,
                        KEY_BACKSPACE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT})
            if (ioctl(m_fd, UI_SET_KEYBIT, key) < 0) return fail();
        for (int key : TextCodes)
            if (ioctl(m_fd, UI_SET_KEYBIT, key) < 0) return fail();
        if (ioctl(m_fd, UI_SET_KEYBIT, KEY_LEFTSHIFT) < 0) return fail();
        for (int axis : {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL})
            if (ioctl(m_fd, UI_SET_RELBIT, axis) < 0) return fail();
    } else {
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
    }
    uinput_setup device{};
    std::strcpy(device.name, desktop ? "Barista GamePad Desktop" : "Barista Wii U GamePad");
    device.id.bustype = BUS_VIRTUAL; device.id.vendor = 0x057e; device.id.product = 0; device.id.version = 1;
    device.ff_effects_max = desktop ? 0 : m_effects.size();
    if (ioctl(m_fd, UI_DEV_SETUP, &device) < 0 || ioctl(m_fd, UI_DEV_CREATE) < 0) return fail();
    return Submit({});
}
bool UinputOutput::Submit(const ControllerState& state)
{
    if (m_fd < 0) return false;
    if (m_desktop) return SubmitDesktop(state);
    std::vector<input_event> events;
    auto add = [&](int type, int code, int value) {
        input_event event{}; event.type = type; event.code = code; event.value = value; events.push_back(event);
    };
    for (auto [button, mask] : Buttons) add(EV_KEY, button, !!(state.buttons & mask));
    for (size_t i = 0; i < 4; ++i) add(EV_ABS, Axes[i], state.sticks[i]);
    add(EV_ABS, ABS_Z, state.buttons & 0x80 ? 255 : 0);
    add(EV_ABS, ABS_RZ, state.buttons & 0x40 ? 255 : 0);
    add(EV_ABS, ABS_HAT0X, !!(state.buttons & 0x400) - !!(state.buttons & 0x800));
    add(EV_ABS, ABS_HAT0Y, !!(state.buttons & 0x100) - !!(state.buttons & 0x200));
    add(EV_SYN, SYN_REPORT, 0);
    const auto bytes = events.size() * sizeof(input_event);
    if (write(m_fd, events.data(), bytes) != static_cast<ssize_t>(bytes)) { Stop(); return false; }
    return true;
}

bool UinputOutput::TypeDesktopText(std::string_view text)
{
    if (m_fd < 0 || !m_desktop || text.size() > 1024 || !m_text.empty()) return false;
    for (char ch : text)
        if (ch < 32 || ch > 126) return false;
    m_text.insert(m_text.end(), text.begin(), text.end());
    return true;
}

bool UinputOutput::SubmitDesktop(const ControllerState& state)
{
    const auto now = std::chrono::steady_clock::now();
    const auto mapped = m_mapper.Map(state, std::chrono::duration<double>(now - m_lastSubmit).count());
    m_lastSubmit = now;
    std::vector<input_event> events;
    auto add = [&](int type, int code, int value) {
        input_event event{}; event.type = type; event.code = code; event.value = value; events.push_back(event);
    };
    for (auto [code, value] : {std::pair{REL_X, mapped.x}, {REL_Y, mapped.y},
                              {REL_WHEEL, mapped.wheel}, {REL_HWHEEL, mapped.horizontalWheel}})
        if (value) add(EV_REL, code, value);
    auto key = [&](int code, bool value, bool previous) { if (value != previous) add(EV_KEY, code, value); };
    key(BTN_LEFT, mapped.left, m_lastDesktop.left); key(BTN_RIGHT, mapped.right, m_lastDesktop.right);
    key(BTN_MIDDLE, mapped.middle, m_lastDesktop.middle); key(KEY_ENTER, mapped.enter, m_lastDesktop.enter);
    key(KEY_ESC, mapped.escape, m_lastDesktop.escape); key(KEY_TAB, mapped.tab, m_lastDesktop.tab);
    key(KEY_BACKSPACE, mapped.backspace, m_lastDesktop.backspace); key(KEY_UP, mapped.up, m_lastDesktop.up);
    key(KEY_DOWN, mapped.down, m_lastDesktop.down); key(KEY_LEFT, mapped.previous, m_lastDesktop.previous);
    key(KEY_RIGHT, mapped.next, m_lastDesktop.next);
    if (!m_text.empty())
    {
        // One character per input tick avoids flooding compositor event queues.
        const char ch = m_text.front();
        m_text.pop_front();
        const auto* plain = std::strchr(TextPlain, ch);
        const auto* shifted = std::strchr(TextShifted, ch);
        const int code = TextCodes[plain ? plain - TextPlain : shifted - TextShifted];
        if (!plain) add(EV_KEY, KEY_LEFTSHIFT, 1);
        add(EV_KEY, code, 1); add(EV_SYN, SYN_REPORT, 0);
        add(EV_KEY, code, 0);
        if (!plain) add(EV_KEY, KEY_LEFTSHIFT, 0);
    }
    if (events.empty()) return true;
    add(EV_SYN, SYN_REPORT, 0);
    if (write(m_fd, events.data(), events.size() * sizeof(input_event)) !=
        static_cast<ssize_t>(events.size() * sizeof(input_event))) { Stop(); return false; }
    m_lastDesktop = mapped;
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
    m_text.clear();
    m_mapper.Reset(); m_lastDesktop = {}; m_desktop = false;
}
}
