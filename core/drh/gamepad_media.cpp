#include "drh/gamepad_media.h"

#include <algorithm>
#include <array>

namespace barista::drh
{
namespace
{
constexpr size_t kVideoHeaderSize = 16;
constexpr size_t kAudioHeaderSize = 8;

// The stream parameters the camera's slices are coded with, which its packets
// leave implied: High profile, level 3.2, 4:2:0, 8-bit frame numbers, picture
// order count type 2, one reference frame and 40x30 macroblocks. This is the
// SPS drc-sim uses to decode the console's 864x480 video with the camera's
// size, and the PPS is drc-sim's unchanged (CABAC).
constexpr std::array<uint8_t, 14> kCameraSps{0x00, 0x00, 0x00, 0x01,
	0x67, 0x64, 0x00, 0x20, 0xac, 0x2b, 0x40, 0x50, 0x1e, 0xc8};
constexpr std::array<uint8_t, 9> kCameraPps{0x00, 0x00, 0x00, 0x01,
	0x68, 0xee, 0x06, 0x0c, 0xe8};

// The slice header the packets omit: NAL header, first macroblock 0, an I or P
// slice, PPS 0, the frame number in its 8 bits, and for an IDR its picture ID,
// ending in the CABAC alignment ones so the slice data starts byte aligned.
uint32_t SliceHeader(bool idr, uint8_t frame_number)
{
	return idr ? 0x25b804ffU : 0x21e003ffU | (static_cast<uint32_t>(frame_number) << 13);
}

uint16_t VideoSequence(std::span<const uint8_t> packet)
{
	return static_cast<uint16_t>(((packet[0] & 0x03) << 8) | packet[1]);
}

size_t VideoPayloadSize(std::span<const uint8_t> packet)
{
	return (static_cast<size_t>(packet[2] & 0x07) << 8) | packet[3];
}

bool VideoPacketIsIdr(std::span<const uint8_t> packet)
{
	// Option 0x80 in the 8-byte extended header marks an IDR frame.
	const auto options = packet.subspan(8, 8);
	return std::find(options.begin(), options.end(), 0x80) != options.end();
}

void AppendSlice(std::vector<uint8_t>& out, bool idr, uint8_t frame_number,
	std::span<const uint8_t> data)
{
	const uint32_t header = SliceHeader(idr, frame_number);
	out.insert(out.end(), {0x00, 0x00, 0x00, 0x01,
		static_cast<uint8_t>(header >> 24), static_cast<uint8_t>(header >> 16),
		static_cast<uint8_t>(header >> 8), static_cast<uint8_t>(header)});
	// Emulation prevention, which the GamePad's own stream does without.
	int zeros = 0;
	for (const uint8_t byte : data)
	{
		if (zeros >= 2 && byte <= 3)
		{
			out.push_back(0x03);
			zeros = 0;
		}
		out.push_back(byte);
		zeros = byte == 0 ? zeros + 1 : 0;
	}
}
}

std::optional<CameraFrame> CameraStreamAssembler::push(std::span<const uint8_t> packet)
{
	if (packet.size() < kVideoHeaderSize || (packet[0] & 0xf0) != 0xf0)
		return std::nullopt;
	const size_t payload_size = VideoPayloadSize(packet);
	if (kVideoHeaderSize + payload_size > packet.size())
		return std::nullopt;

	const uint16_t sequence = VideoSequence(packet);
	const bool lost = m_last_sequence && sequence != ((*m_last_sequence + 1) & 0x3ff);
	m_last_sequence = sequence;
	if (lost && m_in_frame)
	{
		m_in_frame = false;
		++m_dropped_frames;
	}

	const bool frame_begin = (packet[2] & 0x40) != 0;
	const bool frame_end = (packet[2] & 0x10) != 0;
	if (frame_begin)
	{
		m_frame.clear();
		m_in_frame = true;
		m_frame_idr = VideoPacketIsIdr(packet);
	}
	if (!m_in_frame)
		return std::nullopt;
	const auto payload = packet.subspan(kVideoHeaderSize, payload_size);
	m_frame.insert(m_frame.end(), payload.begin(), payload.end());
	if (!frame_end)
		return std::nullopt;

	m_in_frame = false;
	if (m_frame_idr)
	{
		m_have_idr = true;
		m_frame_number = 0;
	}
	else if (!m_have_idr)
	{
		++m_dropped_frames;
		return std::nullopt;
	}
	CameraFrame frame;
	frame.idr = m_frame_idr;
	frame.timestamp = static_cast<uint32_t>(packet[4]) << 24 |
		static_cast<uint32_t>(packet[5]) << 16 | static_cast<uint32_t>(packet[6]) << 8 | packet[7];
	frame.annex_b.reserve(kCameraSps.size() + kCameraPps.size() + m_frame.size() + m_frame.size() / 64 + 8);
	if (frame.idr)
	{
		frame.annex_b.insert(frame.annex_b.end(), kCameraSps.begin(), kCameraSps.end());
		frame.annex_b.insert(frame.annex_b.end(), kCameraPps.begin(), kCameraPps.end());
	}
	AppendSlice(frame.annex_b, frame.idr, m_frame_number++, m_frame);
	return frame;
}

void CameraStreamAssembler::reset()
{
	m_frame.clear();
	m_in_frame = false;
	m_have_idr = false;
	m_last_sequence.reset();
}

std::optional<std::span<const uint8_t>> MicrophonePcm(std::span<const uint8_t> packet)
{
	// Header bits, most significant first: format (3), mono, vibrate, type
	// (0 for audio, 1 for the video-format packets), a 10-bit sequence, then a
	// big-endian payload size and a timestamp the GamePad leaves at zero.
	if (packet.size() <= kAudioHeaderSize || (packet[0] & 0x04) != 0)
		return std::nullopt;
	const size_t payload_size = (static_cast<size_t>(packet[2]) << 8) | packet[3];
	if (payload_size == 0 || payload_size % 2 != 0 || kAudioHeaderSize + payload_size != packet.size())
		return std::nullopt;
	return packet.subspan(kAudioHeaderSize, payload_size);
}
}
