#include "drh/encoder/gamepad_home_menu.h"

#include "drh/encoder/encoder.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

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

struct Color
{
	uint8_t red;
	uint8_t green;
	uint8_t blue;
};

constexpr Color kCream{250, 244, 235};
constexpr Color kWarmWhite{255, 251, 246};
constexpr Color kBrown{54, 36, 26};
constexpr Color kMidBrown{105, 75, 59};
constexpr Color kMuted{139, 116, 101};
constexpr Color kBorder{226, 211, 197};
constexpr Color kBlue{25, 166, 224};
constexpr Color kGreen{31, 171, 108};
constexpr Color kRed{205, 79, 68};

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

struct TouchPoint
{
	bool pressed = false;
	int x = 0;
	int y = 0;
};

TouchPoint ReadTouch(std::span<const uint8_t> report)
{
	if (report.size() != 128)
		return {};
	int raw_x = 0;
	int raw_y = 0;
	for (size_t point = 0; point < 10; ++point)
	{
		const size_t base = 36 + point * 4;
		raw_x += ((report[base + 1] & 0x0f) << 8) | report[base];
		raw_y += ((report[base + 3] & 0x0f) << 8) | report[base + 2];
	}
	raw_x /= 10;
	raw_y /= 10;
	int pressure = 0;
	for (size_t point = 0; point < 4; ++point)
		pressure |= ((report[37 + point * 4] >> 4) & 7) << (point * 3);
	if (pressure == 0)
		return {};

	// These are the GamePad's built-in pre-UIC calibration points, also used
	// by libdrc. Convert the raw 12-bit samples to the 854x480 touch surface.
	const int calibrated_x = 20 + (raw_x - 195) * (834 - 20) / (3877 - 195);
	const int calibrated_y = 20 + (raw_y - 3818) * (460 - 20) / (373 - 3818);
	return {true,
		std::clamp(calibrated_x, 0, 853) * static_cast<int>(kWidth - 1) / 853,
		std::clamp(calibrated_y, 0, 479)};
}

void ClearButtons(std::span<uint8_t> report)
{
	if (report.size() <= 80)
		return;
	report[2] = 0;
	report[3] = 0;
	report[80] = 0;
}

void ClearTouch(std::span<uint8_t> report)
{
	if (report.size() != 128)
		return;
	std::fill(report.begin() + 36, report.begin() + 76, 0);
}

class Canvas
{
public:
	Canvas() : m_pixels(kWidth * kHeight * 3) {}

	void clear(Color color)
	{
		for (size_t offset = 0; offset < m_pixels.size(); offset += 3)
		{
			m_pixels[offset] = color.red;
			m_pixels[offset + 1] = color.green;
			m_pixels[offset + 2] = color.blue;
		}
	}

	void pixel(int x, int y, Color color, uint8_t alpha = 255)
	{
		if (x < 0 || y < 0 || x >= static_cast<int>(kWidth) || y >= static_cast<int>(kHeight))
			return;
		const size_t offset = (static_cast<size_t>(y) * kWidth + x) * 3;
		for (int channel = 0; channel < 3; ++channel)
		{
			const int source = channel == 0 ? color.red : channel == 1 ? color.green : color.blue;
			m_pixels[offset + channel] = static_cast<uint8_t>((source * alpha +
				m_pixels[offset + channel] * (255 - alpha) + 127) / 255);
		}
	}

	void rounded_rect(int x, int y, int width, int height, int radius, Color color,
		uint8_t alpha = 255)
	{
		if (radius <= 0)
		{
			for (int py = y; py < y + height; ++py)
				for (int px = x; px < x + width; ++px)
					pixel(px, py, color, alpha);
			return;
		}
		for (int py = y; py < y + height; ++py)
		{
			for (int px = x; px < x + width; ++px)
			{
				const float center_x = std::clamp(px + 0.5f,
					static_cast<float>(x + radius), static_cast<float>(x + width - radius));
				const float center_y = std::clamp(py + 0.5f,
					static_cast<float>(y + radius), static_cast<float>(y + height - radius));
				const float dx = px + 0.5f - center_x;
				const float dy = py + 0.5f - center_y;
				const float coverage = std::clamp(radius + 0.5f - std::sqrt(dx * dx + dy * dy),
					0.0f, 1.0f);
				pixel(px, py, color, static_cast<uint8_t>(alpha * coverage));
			}
		}
	}

	void stroked_rounded_rect(int x, int y, int width, int height, int radius,
		int thickness, Color stroke, Color fill)
	{
		rounded_rect(x, y, width, height, radius, stroke);
		rounded_rect(x + thickness, y + thickness, width - thickness * 2,
			height - thickness * 2, std::max(1, radius - thickness), fill);
	}

	void circle(int center_x, int center_y, int radius, Color color, uint8_t alpha = 255)
	{
		for (int y = center_y - radius - 1; y <= center_y + radius + 1; ++y)
			for (int x = center_x - radius - 1; x <= center_x + radius + 1; ++x)
			{
				const float dx = x + 0.5f - center_x;
				const float dy = y + 0.5f - center_y;
				const float coverage = std::clamp(radius + 0.5f - std::sqrt(dx * dx + dy * dy),
					0.0f, 1.0f);
				pixel(x, y, color, static_cast<uint8_t>(alpha * coverage));
			}
	}

	void line(float x1, float y1, float x2, float y2, float width, Color color,
		uint8_t alpha = 255)
	{
		const float vx = x2 - x1;
		const float vy = y2 - y1;
		const float length_squared = vx * vx + vy * vy;
		const int left = static_cast<int>(std::floor(std::min(x1, x2) - width));
		const int right = static_cast<int>(std::ceil(std::max(x1, x2) + width));
		const int top = static_cast<int>(std::floor(std::min(y1, y2) - width));
		const int bottom = static_cast<int>(std::ceil(std::max(y1, y2) + width));
		for (int y = top; y <= bottom; ++y)
			for (int x = left; x <= right; ++x)
			{
				const float projection = length_squared == 0 ? 0 : std::clamp(
					((x + 0.5f - x1) * vx + (y + 0.5f - y1) * vy) / length_squared,
					0.0f, 1.0f);
				const float dx = x + 0.5f - (x1 + projection * vx);
				const float dy = y + 0.5f - (y1 + projection * vy);
				const float coverage = std::clamp(width / 2 + 0.5f -
					std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
				pixel(x, y, color, static_cast<uint8_t>(alpha * coverage));
			}
	}

	void arc(float center_x, float center_y, float radius, float start, float end,
		float width, Color color, uint8_t alpha = 255)
	{
		constexpr int kSegments = 18;
		float previous_x = center_x + std::cos(start) * radius;
		float previous_y = center_y + std::sin(start) * radius;
		for (int segment = 1; segment <= kSegments; ++segment)
		{
			const float angle = start + (end - start) * segment / kSegments;
			const float x = center_x + std::cos(angle) * radius;
			const float y = center_y + std::sin(angle) * radius;
			line(previous_x, previous_y, x, y, width, color, alpha);
			previous_x = x;
			previous_y = y;
		}
	}

	void rgba_image(int x, int y, int width, int height, std::span<const uint8_t> rgba)
	{
		if (rgba.size() != static_cast<size_t>(width * height * 4))
			return;
		for (int row = 0; row < height; ++row)
			for (int column = 0; column < width; ++column)
			{
				const size_t offset = static_cast<size_t>(row * width + column) * 4;
				pixel(x + column, y + row,
					{rgba[offset], rgba[offset + 1], rgba[offset + 2]}, rgba[offset + 3]);
			}
	}

	int text_width(FT_Face face, std::string_view text, int size)
	{
		if (face == nullptr || FT_Set_Pixel_Sizes(face, 0, size) != 0)
			return 0;
		int width = 0;
		FT_UInt previous = 0;
		for (const unsigned char ch : text)
		{
			const FT_UInt index = FT_Get_Char_Index(face, ch);
			if (previous != 0 && index != 0 && FT_HAS_KERNING(face))
			{
				FT_Vector kerning{};
				FT_Get_Kerning(face, previous, index, FT_KERNING_DEFAULT, &kerning);
				width += static_cast<int>(kerning.x >> 6);
			}
			if (FT_Load_Glyph(face, index, FT_LOAD_DEFAULT) == 0)
				width += static_cast<int>(face->glyph->advance.x >> 6);
			previous = index;
		}
		return width;
	}

	void text(FT_Face face, int x, int baseline, std::string_view value, int size,
		Color color, uint8_t alpha = 255)
	{
		if (face == nullptr || FT_Set_Pixel_Sizes(face, 0, size) != 0)
			return;
		FT_UInt previous = 0;
		for (const unsigned char ch : value)
		{
			const FT_UInt index = FT_Get_Char_Index(face, ch);
			if (previous != 0 && index != 0 && FT_HAS_KERNING(face))
			{
				FT_Vector kerning{};
				FT_Get_Kerning(face, previous, index, FT_KERNING_DEFAULT, &kerning);
				x += static_cast<int>(kerning.x >> 6);
			}
			if (FT_Load_Glyph(face, index, FT_LOAD_DEFAULT) != 0 ||
				FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0)
				continue;
			const FT_Bitmap& bitmap = face->glyph->bitmap;
			for (unsigned row = 0; row < bitmap.rows; ++row)
				for (unsigned column = 0; column < bitmap.width; ++column)
				{
					const uint8_t coverage = bitmap.buffer[row * bitmap.pitch + column];
					pixel(x + face->glyph->bitmap_left + static_cast<int>(column),
						baseline - face->glyph->bitmap_top + static_cast<int>(row), color,
						static_cast<uint8_t>(coverage * alpha / 255));
				}
			x += static_cast<int>(face->glyph->advance.x >> 6);
			previous = index;
		}
	}

	void to_i420(std::span<uint8_t> output) const
	{
		if (output.size() < DrcVideoFrameBytes)
			return;
		const size_t u_offset = kWidth * kHeight;
		const size_t v_offset = u_offset + kWidth * kHeight / 4;
		for (size_t y = 0; y < kHeight; ++y)
			for (size_t x = 0; x < kWidth; ++x)
			{
				const size_t rgb = (y * kWidth + x) * 3;
				output[y * kWidth + x] = luma(m_pixels[rgb], m_pixels[rgb + 1],
					m_pixels[rgb + 2]);
			}
		for (size_t y = 0; y < kHeight; y += 2)
			for (size_t x = 0; x < kWidth; x += 2)
			{
				int red = 0;
				int green = 0;
				int blue = 0;
				for (size_t row = 0; row < 2; ++row)
					for (size_t column = 0; column < 2; ++column)
					{
						const size_t rgb = ((y + row) * kWidth + x + column) * 3;
						red += m_pixels[rgb];
						green += m_pixels[rgb + 1];
						blue += m_pixels[rgb + 2];
					}
				const size_t chroma = (y / 2) * (kWidth / 2) + x / 2;
				output[u_offset + chroma] = chroma_u(red / 4, green / 4, blue / 4);
				output[v_offset + chroma] = chroma_v(red / 4, green / 4, blue / 4);
			}
	}

private:
	static uint8_t luma(int red, int green, int blue)
	{
		return static_cast<uint8_t>(std::clamp(((66 * red + 129 * green + 25 * blue +
			128) >> 8) + 16, 16, 235));
	}

	static uint8_t chroma_u(int red, int green, int blue)
	{
		return static_cast<uint8_t>(std::clamp(((-38 * red - 74 * green + 112 * blue +
			128) >> 8) + 128, 16, 240));
	}

	static uint8_t chroma_v(int red, int green, int blue)
	{
		return static_cast<uint8_t>(std::clamp(((112 * red - 94 * green - 18 * blue +
			128) >> 8) + 128, 16, 240));
	}

	std::vector<uint8_t> m_pixels;
};

void DrawCoffeeMark(Canvas& canvas)
{
	canvas.circle(40, 35, 21, kWarmWhite, 28);
	canvas.rounded_rect(27, 31, 22, 13, 4, kCream);
	canvas.rounded_rect(30, 28, 16, 4, 2, kCream);
	canvas.circle(51, 36, 7, kCream);
	canvas.circle(51, 36, 4, kBrown);
	canvas.line(28, 47, 51, 47, 3, kCream);
	canvas.line(34, 26, 32, 21, 2, kCream, 180);
	canvas.line(41, 26, 43, 20, 2, kCream, 180);
}

void DrawSun(Canvas& canvas, int x, int y, Color color)
{
	canvas.circle(x, y, 9, color);
	for (int index = 0; index < 8; ++index)
	{
		const float angle = static_cast<float>(index) * 3.14159265f / 4;
		canvas.line(x + std::cos(angle) * 15, y + std::sin(angle) * 15,
			x + std::cos(angle) * 22, y + std::sin(angle) * 22, 3, color);
	}
}

void DrawRumble(Canvas& canvas, int x, int y, Color color)
{
	// A recognizable controller body surrounded by symmetric vibration waves.
	canvas.circle(x - 13, y + 7, 10, color);
	canvas.circle(x + 13, y + 7, 10, color);
	canvas.rounded_rect(x - 21, y - 9, 42, 25, 10, color);
	canvas.line(x - 14, y + 2, x - 6, y + 2, 2, kWarmWhite);
	canvas.line(x - 10, y - 2, x - 10, y + 6, 2, kWarmWhite);
	canvas.circle(x + 9, y - 1, 2, kWarmWhite);
	canvas.circle(x + 14, y + 4, 2, kWarmWhite);
	canvas.arc(x - 19, y + 2, 10, 2.15f, 4.13f, 2.5f, color, 180);
	canvas.arc(x - 19, y + 2, 16, 2.25f, 4.03f, 2.5f, color, 120);
	canvas.arc(x + 19, y + 2, 10, -1.0f, 1.0f, 2.5f, color, 180);
	canvas.arc(x + 19, y + 2, 16, -0.9f, 0.9f, 2.5f, color, 120);
}

void DrawBattery(Canvas& canvas, int x, int y, int percent)
{
	const Color level = percent <= 20 ? kRed : kGreen;
	canvas.stroked_rounded_rect(x, y, 48, 22, 6, 2, kCream, kBrown);
	canvas.rounded_rect(x + 48, y + 7, 4, 8, 2, kCream);
	const int width = 38 * std::clamp(percent, 0, 100) / 100;
	if (width > 0)
		canvas.rounded_rect(x + 5, y + 5, width, 12, 3, level);
}
}

struct GamepadHomeMenu::FontData
{
	FT_Library library = nullptr;
	FT_Face regular = nullptr;
	FT_Face semibold = nullptr;
	std::vector<uint8_t> logo;

	FontData()
	{
		if (FT_Init_FreeType(&library) != 0)
			return;
		const char* configured = std::getenv("BARISTA_HOME_MENU_FONT_DIR");
		const std::filesystem::path directory = configured && *configured
			? configured : BARISTA_HOME_MENU_DEV_FONT_DIR;
		const std::array<std::pair<FT_Face*, std::array<std::filesystem::path, 2>>, 2> faces{{
			{&regular, {BARISTA_HOME_MENU_FONT_REGULAR, directory / "Jost-Regular.ttf"}},
			{&semibold, {BARISTA_HOME_MENU_FONT_SEMIBOLD, directory / "Jost-SemiBold.ttf"}},
		}};
		for (const auto& [face, paths] : faces)
			for (const auto& path : paths)
				if (FT_New_Face(library, path.c_str(), 0, face) == 0)
					break;
		for (const std::filesystem::path path : {std::filesystem::path(BARISTA_HOME_MENU_LOGO),
			std::filesystem::path(BARISTA_HOME_MENU_DEV_LOGO)})
		{
			std::ifstream input(path, std::ios::binary);
			logo.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
			if (logo.size() == 56 * 56 * 4)
				break;
			logo.clear();
		}
	}

	~FontData()
	{
		if (regular != nullptr)
			FT_Done_Face(regular);
		if (semibold != nullptr)
			FT_Done_Face(semibold);
		if (library != nullptr)
			FT_Done_FreeType(library);
	}
};

GamepadHomeMenu::GamepadHomeMenu() : m_fonts(std::make_unique<FontData>()) {}
GamepadHomeMenu::~GamepadHomeMenu() = default;

HomeMenuUpdate GamepadHomeMenu::process_input(std::span<uint8_t> report)
{
	HomeMenuUpdate update;
	const uint32_t buttons = ReadButtons(report);
	const TouchPoint touch = ReadTouch(report);
	std::lock_guard lock(m_mutex);
	const uint32_t pressed = buttons & ~m_previous_buttons;
	m_previous_buttons = buttons;
	const bool touch_started = touch.pressed && !m_touch_pressed;
	m_touch_pressed = touch.pressed;
	const bool was_open = m_open.load();
	bool is_open = was_open;
	bool touch_changed = false;

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

		if (touch.pressed && touch.y >= 139 && touch.y < 245)
		{
			if (m_selected_row != 0)
			{
				m_selected_row = 0;
				touch_changed = true;
			}
			if (touch.x >= 590 && touch.x <= 812)
			{
				const uint8_t level = static_cast<uint8_t>(std::clamp(
					(touch.x - 590) * 5 / 222 + 1, 1, 5));
				if (level != m_brightness)
				{
					m_brightness = level;
					update.brightness = level;
					touch_changed = true;
				}
			}
		}
		else if (touch_started && touch.y >= 259 && touch.y < 365)
		{
			m_selected_row = 1;
			m_rumble_enabled = !m_rumble_enabled;
			touch_changed = true;
		}
		else if (touch_started && touch.y >= 403 && touch.y < 465 &&
			touch.x >= 530)
		{
			is_open = false;
			touch_changed = true;
		}
	}

	const bool controls_changed = pressed & (kButtonUp | kButtonDown | kButtonLeft |
		kButtonRight | kButtonA | kButtonB | kButtonHome);
	update.display_changed = was_open != is_open || touch_changed ||
		(is_open && controls_changed != 0);
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
	bool consume_touch = was_open || is_open || m_touch_captured;
	if (touch.pressed && (was_open || is_open))
	{
		m_touch_captured = true;
		consume_touch = true;
	}
	else if (!touch.pressed && m_touch_captured)
	{
		m_touch_captured = false;
	}
	if (consume_touch)
		ClearTouch(report);
	return update;
}

bool GamepadHomeMenu::rumble_active() const
{
	return SteadyMilliseconds() < m_rumble_until_ms.load();
}

bool GamepadHomeMenu::rumble_enabled() const
{
	std::lock_guard lock(m_mutex);
	return m_rumble_enabled;
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
	if (opacity == 0 || frame.size() < DrcVideoFrameBytes)
		return;
	std::lock_guard lock(m_mutex);
	const uint64_t revision = m_revision.load();
	if (m_cached_menu.size() != DrcVideoFrameBytes || m_cached_revision != revision ||
		m_cached_battery_valid != battery_valid || m_cached_battery != battery_charge)
	{
		Canvas canvas;
		canvas.clear(kCream);
		canvas.rounded_rect(0, 0, kWidth, 72, 0, kBrown);
		if (m_fonts->logo.size() == 56 * 56 * 4)
			canvas.rgba_image(12, 8, 56, 56, m_fonts->logo);
		else
			DrawCoffeeMark(canvas);
		canvas.text(m_fonts->semibold, 72, 45, "Barista", 28, kWarmWhite);
		canvas.text(m_fonts->regular, 342, 43, "GamePad quick settings", 19,
			Color{226, 210, 198});

		const int battery_percent = battery_valid
			? std::clamp((static_cast<int>(battery_charge) * 100 + 88) / 176, 0, 100) : 0;
		const std::string battery_text = battery_valid
			? std::to_string(battery_percent) + "%" : "--";
		const int battery_text_width = canvas.text_width(m_fonts->semibold, battery_text, 18);
		canvas.text(m_fonts->semibold, 744 - battery_text_width, 43, battery_text, 18,
			kWarmWhite);
		DrawBattery(canvas, 758, 24, battery_percent);

		canvas.text(m_fonts->semibold, 42, 112, "GamePad", 28, kBrown);
		canvas.text(m_fonts->regular, 178, 110, "Adjust settings without leaving your game",
			16, kMuted);

		auto card = [&](int y, bool selected) {
			canvas.rounded_rect(46, y + 6, 772, 106, 18, kBrown, 18);
			if (selected)
				canvas.stroked_rounded_rect(42, y, 780, 106, 18, 3, kBlue, kWarmWhite);
			else
				canvas.stroked_rounded_rect(42, y, 780, 106, 18, 1, kBorder, kWarmWhite);
		};

		card(139, m_selected_row == 0);
		canvas.circle(91, 192, 29, m_selected_row == 0 ? Color{226, 246, 253} : kCream);
		DrawSun(canvas, 91, 192, m_selected_row == 0 ? kBlue : kMidBrown);
		canvas.text(m_fonts->semibold, 138, 184, "Screen brightness", 22, kBrown);
		canvas.text(m_fonts->regular, 138, 211, "Use LEFT and RIGHT to adjust", 15, kMuted);
		for (int level = 1; level <= 5; ++level)
		{
			const int height = 20 + level * 7;
			canvas.rounded_rect(609 + (level - 1) * 37, 214 - height, 24, height, 7,
				level <= m_brightness ? kBlue : Color{222, 211, 201});
		}

		card(259, m_selected_row == 1);
		canvas.circle(91, 312, 29, m_selected_row == 1 ? Color{226, 246, 253} : kCream);
		DrawRumble(canvas, 91, 312, m_selected_row == 1 ? kBlue : kMidBrown);
		canvas.text(m_fonts->semibold, 138, 304, "Rumble", 22, kBrown);
		canvas.text(m_fonts->regular, 138, 331, "Allow vibration from games and apps", 15,
			kMuted);
		const Color toggle_color = m_rumble_enabled ? kGreen : Color{188, 174, 163};
		canvas.rounded_rect(700, 288, 84, 46, 23, toggle_color);
		canvas.circle(m_rumble_enabled ? 761 : 723, 311, 17, kWarmWhite);

		canvas.rounded_rect(42, 403, 780, 52, 16, Color{239, 227, 216});
		auto hint = [&](int x, int button_size, std::string_view button,
			std::string_view label) {
			canvas.rounded_rect(x, 416, button_size, 28, 14, kBrown);
			const int button_width = canvas.text_width(m_fonts->semibold, button, 14);
			canvas.text(m_fonts->semibold, x + (button_size - button_width) / 2, 436,
				button, 14,
				kWarmWhite);
			canvas.text(m_fonts->regular, x + button_size + 10, 436, label, 15, kMidBrown);
		};
		hint(64, 58, "TOUCH", "Tap controls directly");
		hint(390, 28, "A", "Change");
		hint(560, 28, "B", "Close");
		hint(682, 64, "HOME", "Close");

		m_cached_menu.resize(DrcVideoFrameBytes);
		canvas.to_i420(m_cached_menu);
		m_cached_revision = revision;
		m_cached_battery_valid = battery_valid;
		m_cached_battery = battery_charge;
	}

	for (size_t index = 0; index < DrcVideoFrameBytes; ++index)
		frame[index] = static_cast<uint8_t>((static_cast<unsigned>(m_cached_menu[index]) *
			opacity + static_cast<unsigned>(frame[index]) * (255 - opacity) + 127) / 255);
}
}
