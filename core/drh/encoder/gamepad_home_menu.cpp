#include "drh/encoder/gamepad_home_menu.h"

#include "drh/encoder/encoder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace barista::drh
{
namespace
{
constexpr uint32_t kButtonA = 0x8000;
constexpr uint32_t kButtonB = 0x4000;
constexpr uint32_t kButtonLeft = 0x0800;
constexpr uint32_t kButtonRight = 0x0400;
constexpr uint32_t kButtonUp = 0x0200;
constexpr uint32_t kButtonDown = 0x0100;
constexpr uint32_t kButtonHome = 0x0002;
constexpr size_t kWidth = DrcVideoWidth;
constexpr size_t kHeight = DrcVideoHeight;
constexpr int64_t kTransitionDurationMs = 240;

int64_t SteadyMilliseconds()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint32_t ReadButtons(std::span<const uint8_t> report)
{
	if (report.size() <= 80)
		return 0;
	return (static_cast<uint32_t>(report[80]) << 16) |
		(static_cast<uint32_t>(report[2]) << 8) | report[3];
}

void ClearButtons(std::span<uint8_t> report)
{
	if (report.size() <= 80)
		return;
	report[2] = 0;
	report[3] = 0;
	report[80] = 0;
}

struct Color
{
	uint8_t y;
	uint8_t u;
	uint8_t v;
};

Color Yuv(uint8_t red, uint8_t green, uint8_t blue)
{
	const int y = ((66 * red + 129 * green + 25 * blue + 128) >> 8) + 16;
	const int u = ((-38 * red - 74 * green + 112 * blue + 128) >> 8) + 128;
	const int v = ((112 * red - 94 * green - 18 * blue + 128) >> 8) + 128;
	return {static_cast<uint8_t>(std::clamp(y, 16, 235)),
		static_cast<uint8_t>(std::clamp(u, 16, 240)),
		static_cast<uint8_t>(std::clamp(v, 16, 240))};
}

void FillRect(std::span<uint8_t> frame, int x, int y, int width, int height, Color color)
{
	x = std::clamp(x, 0, static_cast<int>(kWidth));
	y = std::clamp(y, 0, static_cast<int>(kHeight));
	width = std::min(width, static_cast<int>(kWidth) - x);
	height = std::min(height, static_cast<int>(kHeight) - y);
	if (width <= 0 || height <= 0 || frame.size() < DrcVideoFrameBytes)
		return;

	for (int row = y; row < y + height; ++row)
		std::fill_n(frame.begin() + row * kWidth + x, width, color.y);
	const size_t u_offset = kWidth * kHeight;
	const size_t v_offset = u_offset + kWidth * kHeight / 4;
	for (int row = y / 2; row < (y + height + 1) / 2; ++row)
	{
		const int first = x / 2;
		const int count = (x + width + 1) / 2 - first;
		std::fill_n(frame.begin() + u_offset + row * (kWidth / 2) + first, count, color.u);
		std::fill_n(frame.begin() + v_offset + row * (kWidth / 2) + first, count, color.v);
	}
}

std::array<uint8_t, 5> Glyph(char ch)
{
	switch (ch)
	{
	case 'A': return {0x7e, 0x11, 0x11, 0x11, 0x7e};
	case 'B': return {0x7f, 0x49, 0x49, 0x49, 0x36};
	case 'C': return {0x3e, 0x41, 0x41, 0x41, 0x22};
	case 'D': return {0x7f, 0x41, 0x41, 0x22, 0x1c};
	case 'E': return {0x7f, 0x49, 0x49, 0x49, 0x41};
	case 'F': return {0x7f, 0x09, 0x09, 0x09, 0x01};
	case 'G': return {0x3e, 0x41, 0x49, 0x49, 0x7a};
	case 'H': return {0x7f, 0x08, 0x08, 0x08, 0x7f};
	case 'I': return {0x41, 0x41, 0x7f, 0x41, 0x41};
	case 'J': return {0x20, 0x40, 0x41, 0x3f, 0x01};
	case 'K': return {0x7f, 0x08, 0x14, 0x22, 0x41};
	case 'L': return {0x7f, 0x40, 0x40, 0x40, 0x40};
	case 'M': return {0x7f, 0x02, 0x0c, 0x02, 0x7f};
	case 'N': return {0x7f, 0x04, 0x08, 0x10, 0x7f};
	case 'O': return {0x3e, 0x41, 0x41, 0x41, 0x3e};
	case 'P': return {0x7f, 0x09, 0x09, 0x09, 0x06};
	case 'Q': return {0x3e, 0x41, 0x51, 0x21, 0x5e};
	case 'R': return {0x7f, 0x09, 0x19, 0x29, 0x46};
	case 'S': return {0x46, 0x49, 0x49, 0x49, 0x31};
	case 'T': return {0x01, 0x01, 0x7f, 0x01, 0x01};
	case 'U': return {0x3f, 0x40, 0x40, 0x40, 0x3f};
	case 'V': return {0x1f, 0x20, 0x40, 0x20, 0x1f};
	case 'W': return {0x7f, 0x20, 0x18, 0x20, 0x7f};
	case 'X': return {0x63, 0x14, 0x08, 0x14, 0x63};
	case 'Y': return {0x03, 0x04, 0x78, 0x04, 0x03};
	case 'Z': return {0x61, 0x51, 0x49, 0x45, 0x43};
	case '0': return {0x3e, 0x51, 0x49, 0x45, 0x3e};
	case '1': return {0x00, 0x42, 0x7f, 0x40, 0x00};
	case '2': return {0x62, 0x51, 0x49, 0x49, 0x46};
	case '3': return {0x22, 0x41, 0x49, 0x49, 0x36};
	case '4': return {0x18, 0x14, 0x12, 0x7f, 0x10};
	case '5': return {0x2f, 0x49, 0x49, 0x49, 0x31};
	case '6': return {0x3e, 0x49, 0x49, 0x49, 0x30};
	case '7': return {0x01, 0x71, 0x09, 0x05, 0x03};
	case '8': return {0x36, 0x49, 0x49, 0x49, 0x36};
	case '9': return {0x06, 0x49, 0x49, 0x49, 0x3e};
	case '/': return {0x60, 0x18, 0x06, 0x01, 0x00};
	case '-': return {0x08, 0x08, 0x08, 0x08, 0x08};
	case ':': return {0x00, 0x36, 0x36, 0x00, 0x00};
	case '%': return {0x63, 0x13, 0x08, 0x64, 0x63};
	default: return {};
	}
}

void DrawText(std::span<uint8_t> frame, int x, int y, std::string_view text, int scale, Color color)
{
	for (const char raw : text)
	{
		const char ch = raw >= 'a' && raw <= 'z' ? static_cast<char>(raw - 'a' + 'A') : raw;
		const auto glyph = Glyph(ch);
		for (int column = 0; column < 5; ++column)
			for (int row = 0; row < 7; ++row)
				if ((glyph[column] >> row) & 1)
					FillRect(frame, x + column * scale, y + row * scale, scale, scale, color);
		x += 6 * scale;
	}
}

void DrawBattery(std::span<uint8_t> frame, int x, int y, int percent, Color foreground,
	Color fill)
{
	FillRect(frame, x, y, 82, 34, foreground);
	FillRect(frame, x + 5, y + 5, 72, 24, Yuv(250, 243, 232));
	FillRect(frame, x + 82, y + 9, 7, 16, foreground);
	FillRect(frame, x + 8, y + 8, 66 * std::clamp(percent, 0, 100) / 100, 18, fill);
}
}

HomeMenuUpdate GamepadHomeMenu::process_input(std::span<uint8_t> report)
{
	HomeMenuUpdate update;
	const uint32_t buttons = ReadButtons(report);
	std::lock_guard lock(m_mutex);
	const uint32_t pressed = buttons & ~m_previous_buttons;
	m_previous_buttons = buttons;
	const bool was_open = m_open.load();
	bool is_open = was_open;

	if ((pressed & kButtonHome) != 0)
		is_open = !is_open;
	else if (is_open && (pressed & kButtonB) != 0)
		is_open = false;

	if (is_open)
	{
		if ((pressed & kButtonUp) != 0 && m_selected_row > 0)
			--m_selected_row;
		if ((pressed & kButtonDown) != 0 && m_selected_row < 1)
			++m_selected_row;

		if (m_selected_row == 0)
		{
			const uint8_t old = m_brightness;
			if ((pressed & kButtonLeft) != 0 && m_brightness > 1)
				--m_brightness;
			if ((pressed & kButtonRight) != 0 && m_brightness < 5)
				++m_brightness;
			if ((pressed & kButtonA) != 0)
				m_brightness = m_brightness == 5 ? 1 : m_brightness + 1;
			if (old != m_brightness)
				update.brightness = m_brightness;
		}
		else if ((pressed & (kButtonLeft | kButtonRight | kButtonA)) != 0)
		{
			m_rumble_enabled = !m_rumble_enabled;
		}
	}

	const bool controls_changed = pressed & (kButtonUp | kButtonDown | kButtonLeft |
		kButtonRight | kButtonA | kButtonB | kButtonHome);
	update.display_changed = was_open != is_open || (is_open && controls_changed != 0);
	if (was_open != is_open)
	{
		const int64_t now = SteadyMilliseconds();
		const int64_t elapsed = std::clamp(now - m_transition_started_ms,
			int64_t{0}, kTransitionDurationMs);
		const int current = m_open.load()
			? m_transition_from + (255 - m_transition_from) * elapsed / kTransitionDurationMs
			: m_transition_from - m_transition_from * elapsed / kTransitionDurationMs;
		m_transition_from = static_cast<uint8_t>(std::clamp(current, 0, 255));
		m_transition_started_ms = now;
		m_open.store(is_open);
		update.play_sound = true;
	}
	if (update.display_changed)
	{
		++m_revision;
		if (m_rumble_enabled)
			m_rumble_until_ms.store(SteadyMilliseconds() + 90);
	}

	if (was_open || is_open || (buttons & kButtonHome) != 0)
		ClearButtons(report);
	return update;
}

bool GamepadHomeMenu::rumble_active() const
{
	return SteadyMilliseconds() < m_rumble_until_ms.load();
}

uint8_t GamepadHomeMenu::opacity() const
{
	std::lock_guard lock(m_mutex);
	if (m_transition_started_ms == 0)
		return m_open.load() ? 255 : 0;
	const int64_t elapsed = std::clamp(SteadyMilliseconds() - m_transition_started_ms,
		int64_t{0}, kTransitionDurationMs);
	const int opacity = m_open.load()
		? m_transition_from + (255 - m_transition_from) * elapsed / kTransitionDurationMs
		: m_transition_from - m_transition_from * elapsed / kTransitionDurationMs;
	return static_cast<uint8_t>(std::clamp(opacity, 0, 255));
}

void GamepadHomeMenu::render(std::span<uint8_t> frame, bool battery_valid,
	uint8_t battery_charge, uint8_t opacity) const
{
	if (opacity == 0)
		return;
	std::vector<uint8_t> source;
	if (opacity != 255)
		source.assign(frame.begin(), frame.end());
	uint8_t selected;
	uint8_t brightness;
	bool rumble;
	{
		std::lock_guard lock(m_mutex);
		selected = m_selected_row;
		brightness = m_brightness;
		rumble = m_rumble_enabled;
	}

	const Color cream = Yuv(250, 243, 232);
	const Color brown = Yuv(56, 38, 26);
	const Color muted = Yuv(139, 113, 94);
	const Color card = Yuv(239, 224, 208);
	const Color selected_color = Yuv(220, 195, 173);
	const Color blue = Yuv(31, 169, 224);
	const Color green = Yuv(28, 170, 104);
	FillRect(frame, 0, 0, kWidth, kHeight, cream);
	FillRect(frame, 0, 0, kWidth, 72, brown);
	DrawText(frame, 42, 24, "BARISTA", 4, cream);
	DrawText(frame, 344, 27, "GAMEPAD MENU", 3, card);

	const int battery_percent = battery_valid
		? std::clamp((static_cast<int>(battery_charge) * 100 + 88) / 176, 0, 100) : 0;
	DrawBattery(frame, 724, 20, battery_percent, cream,
		battery_percent <= 20 ? Yuv(204, 69, 61) : green);
	if (battery_valid)
		DrawText(frame, 650, 30, std::to_string(battery_percent) + "%", 2, cream);

	DrawText(frame, 60, 104, "GAMEPAD SETTINGS", 3, brown);
	FillRect(frame, 50, 145, 764, 96, selected == 0 ? selected_color : card);
	DrawText(frame, 82, 174, "BRIGHTNESS", 3, brown);
	for (int level = 1; level <= 5; ++level)
		FillRect(frame, 574 + level * 35, 174 + (5 - level) * 4, 23, 32 + level * 4,
			level <= brightness ? blue : muted);
	DrawText(frame, 752, 187, std::to_string(brightness), 3, brown);

	FillRect(frame, 50, 255, 764, 96, selected == 1 ? selected_color : card);
	DrawText(frame, 82, 284, "RUMBLE", 3, brown);
	FillRect(frame, 650, 277, 126, 50, rumble ? green : muted);
	DrawText(frame, rumble ? 686 : 675, 292, rumble ? "ON" : "OFF", 3, cream);

	DrawText(frame, 68, 403, "UP/DOWN SELECT", 2, muted);
	DrawText(frame, 315, 403, "LEFT/RIGHT CHANGE", 2, muted);
	DrawText(frame, 630, 403, "B CLOSE", 2, muted);
	DrawText(frame, 305, 447, "HOME ALSO CLOSES THIS MENU", 2, brown);
	if (!source.empty())
	{
		for (size_t index = 0; index < frame.size(); ++index)
			frame[index] = static_cast<uint8_t>((static_cast<unsigned>(frame[index]) * opacity +
				static_cast<unsigned>(source[index]) * (255 - opacity) + 127) / 255);
	}
}
}
