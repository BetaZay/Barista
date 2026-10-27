#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace barista::drh
{
// The GamePad's camera, once RuntimeTransport::set_camera_enabled() has turned
// it on, sends 640x480 H.264 at about 30 frames per second (fewer in low
// light) to the console's video port. Its packets use the same 16-byte header
// as the console's video to the GamePad and, like that stream, carry slice data
// with no NAL or slice header.
constexpr int kCameraWidth = 640;
constexpr int kCameraHeight = 480;

// The GamePad's microphone, once RuntimeTransport::set_microphone_enabled()
// has turned it on: signed 16-bit little-endian mono PCM at 16 kHz, 512 bytes
// every 16 ms, to the console's audio port.
constexpr uint32_t kMicrophoneSampleRate = 16000;

struct CameraFrame
{
	// Annex B H.264 a standard decoder accepts: an SPS and PPS before each IDR
	// frame, then one slice NAL unit with its header restored.
	std::vector<uint8_t> annex_b;
	bool idr = false;
	// The packet header's media timestamp, in microseconds.
	uint32_t timestamp = 0;
};

// Reassembles camera frames from the packets the GamePad sends to the
// console's video port (RuntimeChannel::Video). A frame that loses a packet is
// dropped, as is everything before the first IDR frame.
class CameraStreamAssembler
{
public:
	std::optional<CameraFrame> push(std::span<const uint8_t> packet);
	void reset();
	uint64_t dropped_frames() const { return m_dropped_frames; }

private:
	std::vector<uint8_t> m_frame;
	bool m_in_frame = false;
	bool m_frame_idr = false;
	bool m_have_idr = false;
	std::optional<uint16_t> m_last_sequence;
	uint8_t m_frame_number = 0;
	uint64_t m_dropped_frames = 0;
};

// The PCM of one packet the GamePad sent to the console's audio port, or
// nothing when the packet is not microphone audio.
std::optional<std::span<const uint8_t>> MicrophonePcm(std::span<const uint8_t> packet);
}
