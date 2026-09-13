#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>

namespace barista::drh
{
struct HomeMenuUpdate
{
	bool display_changed = false;
	bool play_sound = false;
	std::optional<uint8_t> brightness;
};

class GamepadHomeMenu
{
public:
	HomeMenuUpdate process_input(std::span<uint8_t> report);
	void render(std::span<uint8_t> i420, bool battery_valid, uint8_t battery_charge,
		uint8_t opacity = 255) const;

	bool open() const { return m_open.load(); }
	uint64_t revision() const { return m_revision.load(); }
	uint8_t opacity() const;
	bool rumble_active() const;

private:
	mutable std::mutex m_mutex;
	std::atomic_bool m_open{false};
	std::atomic_uint64_t m_revision{0};
	std::atomic_int64_t m_rumble_until_ms{0};
	uint32_t m_previous_buttons = 0;
	uint8_t m_selected_row = 0;
	uint8_t m_brightness = 3;
	bool m_rumble_enabled = true;
	int64_t m_transition_started_ms = 0;
	uint8_t m_transition_from = 0;
};
}
