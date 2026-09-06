#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <iosfwd>

namespace drc_host { class RuntimeTransport; }
namespace drc_ipc { class AppHook; }

namespace drcd
{
class MediaStreamer
{
public:
	MediaStreamer(drc_host::RuntimeTransport& transport, std::string path,
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
	class VideoEncoder;
	void video_loop();
	void audio_loop();
	void input_loop();

	drc_host::RuntimeTransport& m_transport;
	std::string m_path;
	bool m_black_frames = false;
	bool m_generated_pattern = false;
	std::unique_ptr<drc_ipc::AppHook> m_bridge;
	std::unique_ptr<VideoEncoder> m_encoder;
	std::thread m_video_thread;
	std::thread m_audio_thread;
	std::thread m_input_thread;
	std::atomic_bool m_stop{false};
	std::atomic_bool m_running{false};
	std::atomic_uint64_t m_audio_packets{0};
};
}
