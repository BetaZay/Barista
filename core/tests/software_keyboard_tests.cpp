#include "drh/encoder/gamepad_home_menu.h"
#include "drh/encoder/encoder.h"
#include "api/controller.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void Buttons(std::array<uint8_t, 128>& report, uint32_t buttons)
{
    report[2] = buttons >> 8; report[3] = buttons; report[80] = buttons >> 16;
}
void Touch(std::array<uint8_t, 128>& report, int x, int y, bool down)
{
    std::fill(report.begin() + 36, report.begin() + 76, 0);
    if (!down) return;
    const int rawX = 195 + x * 853 / 863 * (3877 - 195) / 853;
    const int rawY = 3818 + y * (373 - 3818) / 479;
    for (size_t point = 0; point < 10; ++point)
    {
        const size_t offset = 36 + point * 4;
        report[offset] = rawX; report[offset + 1] = (rawX >> 8) & 15;
        report[offset + 2] = rawY; report[offset + 3] = (rawY >> 8) & 15;
    }
    report[37] |= 0x10;
}
}
int main()
{
    using namespace barista::api;
    barista::drh::GamepadHomeMenu menu;
    std::array<uint8_t, 128> report{};
    auto press = [&](uint32_t buttons)
    {
        Buttons(report, 0); menu.process_input(report);
        Buttons(report, buttons); menu.process_input(report);
        Check(barista::DecodeInput(report).buttons == 0, "keyboard buttons leaked");
        Check(barista::DecodeInput(report).sticks == std::array<int, 4>{}, "keyboard sticks leaked");
    };
    auto tap = [&](int x, int y)
    {
        Touch(report, 0, 0, false); Buttons(report, 0); menu.process_input(report);
        Touch(report, x, y, true); menu.process_input(report);
        Check(std::all_of(report.begin() + 36, report.begin() + 76,
            [](uint8_t value) { return value == 0; }), "keyboard touch leaked");
    };
    Check(menu.show_keyboard({1, "Name your player", "", 3, false}), "open keyboard");
    Check(!menu.show_keyboard({2}), "second request must be busy");
    press(0x8000); // Initial selection q.
    Buttons(report, 0x8000); menu.process_input(report); // Holding A must not repeat.
    press(0x400); // Right, now w.
    press(0x8000);
    press(0x2000); // Shift.
    press(0x8000); // W.
    press(0x8000); // Limit reached.
    press(0x8);
    auto result = menu.take_keyboard_result();
    Check(result && result->id == 1 && result->outcome == KeyboardOutcome::Submitted &&
          result->text == "qwW", "button editing, shift, repeat or limit");
    Buttons(report, 0x8); menu.process_input(report);
    Check(barista::DecodeInput(report).buttons == 0, "closing button leaked while held");
    Buttons(report, 0); menu.process_input(report);
    Check(menu.show_keyboard({2, "Touch typing", "", 100, false}), "touch keyboard");
    tap(75, 265); // q.
    Touch(report, 75, 265, true); menu.process_input(report); // Same contact: one key.
    tap(730, 365); // Backspace.
    tap(75, 365); // Shift.
    tap(75, 265); // Q.
    tap(80, 415); // Symbols.
    tap(75, 215); // !.
    tap(300, 415); // Space.
    tap(740, 415); // Done.
    result = menu.take_keyboard_result();
    Check(result && result->text == "Q! ", "touch editing, symbol page or submission");
    Touch(report, 740, 415, true); menu.process_input(report);
    Check(std::all_of(report.begin() + 36, report.begin() + 76,
        [](uint8_t value) { return value == 0; }), "closing touch leaked before release");
    Touch(report, 0, 0, false); menu.process_input(report);
    Check(menu.show_keyboard({3, "Password", "caf\xc3\xa9", 4, true}), "UTF-8 initial text");
    std::vector<uint8_t> frame(barista::drh::DrcVideoFrameBytes);
    menu.render(frame, true, 88);
    barista::drh::GamepadHomeMenu masked;
    Check(masked.show_keyboard({99, "Password", "1234", 4, true}), "comparison password");
    std::vector<uint8_t> maskedFrame(barista::drh::DrcVideoFrameBytes);
    masked.render(maskedFrame, true, 88);
    Check(frame == maskedFrame, "password pixels disclosed their contents");
    if (const char* path = std::getenv("BARISTA_KEYBOARD_DUMP"))
    {
        std::ofstream dump(path, std::ios::binary);
        dump.write(reinterpret_cast<const char*>(frame.data()), frame.size());
    }
    press(0x4000); // Delete one scalar, not one byte.
    press(0x8);
    result = menu.take_keyboard_result();
    Check(result && result->text == "caf", "UTF-8 backspace");
    Buttons(report, 0); menu.process_input(report);
    Check(menu.show_keyboard({4, "Cancel", "secret", 100, true}), "cancel keyboard");
    menu.cancel_keyboard(99);
    Check(menu.keyboard_open(), "wrong ID cancelled request");
    press(0x2);
    result = menu.take_keyboard_result();
    Check(result && result->outcome == KeyboardOutcome::Cancelled && result->text.empty(), "cancel returned text");
    Check(!menu.show_keyboard({5, "Invalid", std::string("\xc0\x80"), 4}), "invalid UTF-8 accepted");
    Check(!menu.show_keyboard({6, "Too long", "hello", 4}), "oversized initial text accepted");
    std::cout << "software keyboard: touch, navigation, UTF-8, limits, modal input and cancellation passed\n";
}
