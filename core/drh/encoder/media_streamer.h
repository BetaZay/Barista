#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <iosfwd>

namespace barista::drh { class RuntimeTransport; class VideoEncoder; }
namespace barista::drh { class GamepadHomeMenu; }
namespace barista::api { class AppHook; }

namespace barista::drh
{
class MediaStreamer
{
public:
	MediaStreamer(barista::drh::RuntimeTransport& transport, std::string path,
		bool black_frames = false);
	~MediaStreamer();
	MediaStreamer(const MediaStreamer&) = delete;
	MediaStreamer& operator=(const MediaStreamer&) = delete;

	bool start(std::string& error);
	void stop();
	bool running() const { return m_running.load(); }
	static bool protocol_self_test(std::string& error);
	// Offline diagnostic: one IDR/P byte + I420 frame in, five length-prefixed DRH chunks out.
	static bool reencode_replay(std::istream& input, std::ostream& output, std::string& error);

private:
	void video_loop();
	void audio_loop();
	void input_loop();
	void play_home_menu_sound();
	void mix_home_menu_sound(std::span<uint8_t> pcm);

	barista::drh::RuntimeTransport& m_transport;
	std::string m_path;
	bool m_black_frames = false;
	bool m_generated_pattern = false;
	bool m_home_menu_enabled = true;
	std::unique_ptr<barista::api::AppHook> m_bridge;
	std::unique_ptr<VideoEncoder> m_encoder;
	std::unique_ptr<GamepadHomeMenu> m_home_menu;
	std::thread m_video_thread;
	std::thread m_audio_thread;
	std::thread m_input_thread;
	std::atomic_bool m_stop{false};
	std::atomic_bool m_running{false};
	std::atomic_uint64_t m_audio_packets{0};
	std::mutex m_home_menu_sound_mutex;
	std::vector<int16_t> m_home_menu_sound;
	size_t m_home_menu_sound_cursor = 0;
};
}
