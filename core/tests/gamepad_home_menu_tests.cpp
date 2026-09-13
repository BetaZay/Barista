#include "drh/encoder/gamepad_home_menu.h"
#include "drh/encoder/encoder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
void Expect(bool value, const char* message)
{
	if (!value)
		throw std::runtime_error(message);
}

void SetButtons(std::array<uint8_t, 128>& report, uint32_t buttons)
{
	report[2] = static_cast<uint8_t>(buttons >> 8);
	report[3] = static_cast<uint8_t>(buttons);
	report[80] = static_cast<uint8_t>(buttons >> 16);
}

void SetTouch(std::array<uint8_t, 128>& report, int x, int y, bool pressed)
{
	std::fill(report.begin() + 36, report.begin() + 76, 0);
	if (!pressed)
		return;
	const int raw_x = 195 + x * (3877 - 195) / 853;
	const int raw_y = 3818 + y * (373 - 3818) / 479;
	for (size_t point = 0; point < 10; ++point)
	{
		const size_t base = 36 + point * 4;
		report[base] = static_cast<uint8_t>(raw_x);
		report[base + 1] = static_cast<uint8_t>((raw_x >> 8) & 0x0f);
		report[base + 2] = static_cast<uint8_t>(raw_y);
		report[base + 3] = static_cast<uint8_t>((raw_y >> 8) & 0x0f);
	}
	report[37] |= 0x10;
}
}

int main()
{
	barista::drh::GamepadHomeMenu menu;
	std::array<uint8_t, 128> report{};
	SetButtons(report, 0x0002);
	auto update = menu.process_input(report);
	Expect(menu.open(), "HOME did not open the menu");
	Expect(update.display_changed, "opening did not change the display");
	Expect(update.play_sound, "opening did not request its sound effect");
	Expect(menu.opacity() < 255, "opening transition started fully opaque");
	std::this_thread::sleep_for(std::chrono::milliseconds(260));
	Expect(menu.opacity() == 255, "opening transition did not finish");
	Expect(report[2] == 0 && report[3] == 0 && report[80] == 0,
		"HOME leaked through to the application");

	SetButtons(report, 0);
	menu.process_input(report);
	SetButtons(report, 0x0400);
	update = menu.process_input(report);
	Expect(update.brightness == 4, "right did not increase brightness");
	Expect(report[2] == 0 && report[3] == 0, "menu controls leaked to the application");
	Expect(menu.rumble_enabled(), "rumble should default on");

	SetButtons(report, 0);
	menu.process_input(report);
	SetButtons(report, 0x0100);
	menu.process_input(report);
	SetButtons(report, 0);
	menu.process_input(report);
	SetButtons(report, 0x8000);
	menu.process_input(report);
	Expect(!menu.rumble_enabled(), "rumble toggle did not disable vibration");

	SetButtons(report, 0);
	SetTouch(report, 700, 190, true);
	update = menu.process_input(report);
	Expect(update.brightness == 3, "touch did not select a brightness level");
	Expect(std::all_of(report.begin() + 36, report.begin() + 76,
		[](uint8_t value) { return value == 0; }), "menu touch leaked to the application");
	SetTouch(report, 0, 0, false);
	menu.process_input(report);
	SetTouch(report, 400, 312, true);
	menu.process_input(report);
	Expect(menu.rumble_enabled(), "touch did not toggle rumble");
	SetTouch(report, 0, 0, false);
	menu.process_input(report);

	std::vector<uint8_t> frame(barista::drh::DrcVideoFrameBytes, 0);
	menu.render(frame, true, 88);
	Expect(std::any_of(frame.begin(), frame.begin() + barista::drh::DrcVideoWidth *
		barista::drh::DrcVideoHeight, [](uint8_t value) { return value != 0; }),
		"menu did not render a luma plane");
	if (const char* dump = std::getenv("BARISTA_HOME_MENU_DUMP"); dump && *dump)
	{
		std::ofstream output(dump, std::ios::binary | std::ios::trunc);
		output.write(reinterpret_cast<const char*>(frame.data()), frame.size());
		Expect(output.good(), "could not write home menu diagnostic frame");
	}

	SetButtons(report, 0);
	menu.process_input(report);
	SetButtons(report, 0x4000);
	update = menu.process_input(report);
	Expect(!menu.open(), "B did not close the menu");
	Expect(report[2] == 0 && report[3] == 0, "closing B leaked to the application");
	Expect(update.play_sound, "closing did not request its sound effect");
	Expect(menu.opacity() > 0, "closing transition disappeared immediately");
	std::this_thread::sleep_for(std::chrono::milliseconds(260));
	Expect(menu.opacity() == 0, "closing transition did not finish");

	SetButtons(report, 0);
	SetTouch(report, 400, 312, true);
	menu.process_input(report);
	Expect(std::any_of(report.begin() + 36, report.begin() + 76,
		[](uint8_t value) { return value != 0; }), "gameplay touch was consumed while closed");
	SetTouch(report, 0, 0, false);
	menu.process_input(report);
	SetButtons(report, 0x0002);
	menu.process_input(report);
	SetButtons(report, 0);
	menu.process_input(report);
	SetTouch(report, 600, 430, true);
	menu.process_input(report);
	Expect(!menu.open(), "touching the close hint did not close the menu");
	SetTouch(report, 600, 430, true);
	menu.process_input(report);
	Expect(std::all_of(report.begin() + 36, report.begin() + 76,
		[](uint8_t value) { return value == 0; }), "captured touch leaked before release");

	std::cout << "GamePad home menu input and rendering passed\n";
}
