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

	std::cout << "GamePad home menu input and rendering passed\n";
}
