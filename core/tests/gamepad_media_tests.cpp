#include "drh/gamepad_media.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
void expect(bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "test failure: " << message << '\n';
		std::exit(1);
	}
}

// A packet as the GamePad's camera sends it to the console's video port.
std::vector<uint8_t> camera_packet(uint16_t sequence, bool frame_begin, bool frame_end, bool idr,
	std::vector<uint8_t> payload, uint32_t timestamp = 0x02cf078b)
{
	std::vector<uint8_t> packet(16);
	packet[0] = static_cast<uint8_t>(0xf0 | ((sequence >> 8) & 3));
	packet[1] = static_cast<uint8_t>(sequence);
	packet[2] = static_cast<uint8_t>((frame_begin ? 0x40 : 0) | (frame_end ? 0x10 : 0) | 0x08 |
		((payload.size() >> 8) & 7));
	packet[3] = static_cast<uint8_t>(payload.size());
	for (int i = 0; i < 4; ++i)
		packet[4 + i] = static_cast<uint8_t>(timestamp >> (8 * (3 - i)));
	const std::vector<uint8_t> options = idr ?
		std::vector<uint8_t>{0x80, 0x82, 0x02, 0x83, 0x85, 0x1e, 0x00, 0x00} :
		std::vector<uint8_t>{0x82, 0x02, 0x83, 0x85, 0x1e, 0x00, 0x00, 0x00};
	std::copy(options.begin(), options.end(), packet.begin() + 8);
	packet.insert(packet.end(), payload.begin(), payload.end());
	return packet;
}

bool starts_with(const std::vector<uint8_t>& data, const std::vector<uint8_t>& prefix)
{
	return data.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), data.begin());
}

void test_camera_frames()
{
	barista::drh::CameraStreamAssembler assembler;

	// A P frame before any IDR cannot be decoded and is dropped.
	expect(!assembler.push(camera_packet(1, true, true, false, {0x11})).has_value(),
		"a P frame before the first IDR was returned");
	expect(assembler.dropped_frames() == 1, "the P frame before the first IDR was not counted");

	// An IDR frame in two packets gets the SPS, PPS and the IDR slice header,
	// and the escape byte after two zeros.
	expect(!assembler.push(camera_packet(2, true, false, true, {0xaa, 0x00, 0x00})).has_value(),
		"a frame was returned before its last packet");
	const auto idr = assembler.push(camera_packet(3, false, true, true, {0x01, 0xbb}));
	expect(idr.has_value() && idr->idr, "the IDR frame was not returned");
	expect(idr->timestamp == 0x02cf078b, "the frame timestamp was not decoded");
	const std::vector<uint8_t> sps{0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x20, 0xac, 0x2b,
		0x40, 0x50, 0x1e, 0xc8};
	expect(starts_with(idr->annex_b, sps), "the IDR frame does not start with the camera's SPS");
	const std::vector<uint8_t> rest(idr->annex_b.begin() + static_cast<long>(sps.size()), idr->annex_b.end());
	expect(rest == std::vector<uint8_t>{0x00, 0x00, 0x00, 0x01, 0x68, 0xee, 0x06, 0x0c, 0xe8,
		0x00, 0x00, 0x00, 0x01, 0x25, 0xb8, 0x04, 0xff, 0xaa, 0x00, 0x00, 0x03, 0x01, 0xbb},
		"the IDR frame's PPS, slice header or escaped data is wrong");

	// The next P frame gets frame number 1 in its slice header and no SPS.
	const auto p = assembler.push(camera_packet(4, true, true, false, {0x22}));
	expect(p.has_value() && !p->idr, "the P frame after the IDR was not returned");
	expect(p->annex_b == std::vector<uint8_t>{0x00, 0x00, 0x00, 0x01, 0x21, 0xe0, 0x23, 0xff, 0x22},
		"the P frame's slice header is wrong");

	// A frame that loses a packet is dropped; the next whole frame decodes.
	expect(!assembler.push(camera_packet(5, true, false, false, {0x33})).has_value(),
		"a partial frame was returned");
	expect(!assembler.push(camera_packet(7, false, true, false, {0x44})).has_value(),
		"a frame missing a packet was returned");
	expect(assembler.dropped_frames() == 2, "the frame missing a packet was not counted");
	expect(assembler.push(camera_packet(8, true, true, false, {0x55})).has_value(),
		"the frame after a loss was not returned");

	// A gap between frames leaves the next whole frame alone, and sequence
	// numbers wrap at ten bits.
	expect(assembler.push(camera_packet(1023, true, true, false, {0x66})).has_value(),
		"a whole frame after a gap between frames was dropped");
	expect(assembler.push(camera_packet(0, true, true, false, {0x77})).has_value(),
		"the frame after the sequence wrapped was not returned");
	expect(assembler.dropped_frames() == 2, "a frame was counted as dropped across the wrap");

	// Not camera packets.
	expect(!assembler.push(std::vector<uint8_t>(15, 0xf0)).has_value(), "a short packet was accepted");
	auto oversized = camera_packet(1, true, true, false, {0x01});
	oversized[3] = 0x10;
	expect(!assembler.push(oversized).has_value(), "a packet shorter than its payload size was accepted");
}

void test_microphone()
{
	std::vector<uint8_t> packet{0xd0, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
	for (int i = 0; i < 512; ++i)
		packet.push_back(static_cast<uint8_t>(i));
	const auto pcm = barista::drh::MicrophonePcm(packet);
	expect(pcm.has_value() && pcm->size() == 512 && (*pcm)[0] == 0 && (*pcm)[511] == 0xff,
		"the microphone PCM was not found");

	auto format = packet;
	format[0] |= 0x04;
	expect(!barista::drh::MicrophonePcm(format).has_value(), "a video-format packet was taken for audio");
	auto truncated = packet;
	truncated.pop_back();
	expect(!barista::drh::MicrophonePcm(truncated).has_value(), "a truncated packet was accepted");
	expect(!barista::drh::MicrophonePcm(std::vector<uint8_t>(8)).has_value(), "an empty packet was accepted");
}
}

int main()
{
	test_camera_frames();
	test_microphone();
	std::cout << "drc_gamepad_media_tests: ok\n";
	return 0;
}
