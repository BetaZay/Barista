#include "drcd/media_streamer.h"
#include "serial_video_sender.h"

#include "drc_host/runtime_transport.h"
#include "drc_ipc/media_bridge.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <span>
#include <vector>

extern "C" {
#include <x264.h>
}

namespace drcd
{
namespace
{
bool DefaultEnabled(const char* name)
{
	const char* value = std::getenv(name);
	return !value || std::strcmp(value, "0") != 0;
}

int VideoQuantizer()
{
	const char* selected = std::getenv("DRCD_VIDEO_QP");
	if (selected)
	{
		if (std::strcmp(selected, "28") == 0) return 28;
		if (std::strcmp(selected, "32") == 0) return 32;
		if (std::strcmp(selected, "36") == 0) return 36;
		return 32;
	}
	const char* value = std::getenv("DRCD_QP28");
	return value && std::strcmp(value, "1") == 0 ? 28 : 32;
}

constexpr bool VideoInitFlag(bool initialized, bool idr, bool recovery_init)
{
	return !initialized || (recovery_init && idr);
}
static_assert(VideoInitFlag(false, true, false));
static_assert(!VideoInitFlag(true, true, false));
static_assert(VideoInitFlag(true, true, true));
static_assert(!VideoInitFlag(true, false, true));

// The LCD has 854 visible columns, but the DRC H.264 surface is 54 complete
// macroblocks wide.  The GamePad uses a fixed 864x480 decoder configuration
// because SPS/PPS NAL units are not carried in the DRH stream.
constexpr size_t kWidth = 864;
constexpr size_t kHeight = 480;
constexpr size_t kRawFrameSize = kWidth * kHeight * 3 / 2;
constexpr size_t kMaxVideoPayload = 1400;
constexpr size_t kAudioFramesPerPacket = 416;
constexpr size_t kAudioSamplesPerPacket = kAudioFramesPerPacket * 2;
// Real-console PCM: 416 stereo frames at 48 kHz (1664 bytes per packet).
constexpr auto kAudioPacketPeriod = std::chrono::nanoseconds(
	kAudioFramesPerPacket * 1000000000ULL / 48000);
static_assert(kAudioPacketPeriod.count() == 8666666);
constexpr size_t kChunksPerFrame = 5;
constexpr int kMacroblocksPerRow = static_cast<int>(kWidth / 16);
constexpr int kMacroblocksPerChunk = kMacroblocksPerRow * 6;
constexpr int kVideoFpsNumerator = 60000;
constexpr int kVideoFpsDenominator = 1001;
constexpr auto kVideoFramePeriod = std::chrono::microseconds(16683);
constexpr std::array<std::chrono::microseconds, kChunksPerFrame> kChunkSendOffsets{
	std::chrono::microseconds(0), std::chrono::microseconds(3000),
	std::chrono::microseconds(6000), std::chrono::microseconds(9000),
	std::chrono::microseconds(11000)};
static_assert(kChunkSendOffsets.back() < kVideoFramePeriod);

void GeneratePattern(std::span<uint8_t> frame, uint64_t frame_number)
{
	// Neutral chroma, eight legal-range gray bars and a moving bright marker.
	std::fill(frame.begin() + kWidth * kHeight, frame.end(), 128);
	const size_t marker = (frame_number * 4) % kWidth;
	for (size_t y = 0; y < kHeight; ++y)
		for (size_t x = 0; x < kWidth; ++x)
			frame[y * kWidth + x] = (x + kWidth - marker) % kWidth < 16
				? 235 : static_cast<uint8_t>(16 + (x * 8 / kWidth) * 28);
}

void GenerateTone(std::span<uint8_t> pcm, uint32_t& phase)
{
	// 440 Hz, approximately -30 dBFS; explicitly serialize stereo s16le.
	for (size_t offset = 0; offset < pcm.size(); offset += 4)
	{
		const int16_t sample = static_cast<int16_t>(1000 * std::sin(
			6.283185307179586 * phase / 48000.0));
		const uint16_t bits = static_cast<uint16_t>(sample);
		pcm[offset] = pcm[offset + 2] = static_cast<uint8_t>(bits);
		pcm[offset + 1] = pcm[offset + 3] = static_cast<uint8_t>(bits >> 8);
		phase = (phase + 440) % 48000;
	}
}

std::string ShellQuote(const std::string& value)
{
	std::string result = "'";
	for (const char ch : value)
		result += ch == '\'' ? "'\\''" : std::string(1, ch);
	return result + "'";
}

bool ReadExact(FILE* pipe, std::span<uint8_t> output)
{
	size_t offset = 0;
	while (offset < output.size())
	{
		const size_t count = std::fread(output.data() + offset, 1, output.size() - offset, pipe);
		if (count == 0)
			return false;
		offset += count;
	}
	return true;
}

std::vector<uint8_t> BuildAudioPacket(std::span<const uint8_t> pcm, uint16_t sequence, uint32_t timestamp)
{
	std::vector<uint8_t> packet(8 + pcm.size());
	packet[0] = static_cast<uint8_t>(0x20 | ((sequence >> 8) & 3)); // PCM 48 kHz, stereo, audio data
	packet[1] = static_cast<uint8_t>(sequence);
	packet[2] = static_cast<uint8_t>(pcm.size() >> 8);
	packet[3] = static_cast<uint8_t>(pcm.size());
	packet[4] = static_cast<uint8_t>(timestamp);
	packet[5] = static_cast<uint8_t>(timestamp >> 8);
	packet[6] = static_cast<uint8_t>(timestamp >> 16);
	packet[7] = static_cast<uint8_t>(timestamp >> 24);
	std::copy(pcm.begin(), pcm.end(), packet.begin() + 8);
	return packet;
}

std::vector<uint8_t> BuildVideoFormatPacket(uint32_t timestamp)
{
	std::vector<uint8_t> packet(32);
	packet[0] = 0x04; // video-format packet
	// Match all 2,032 authenticated reference format packets: their outer
	// length field is zero despite the 24-byte body. The remaining outer
	// bytes are constant in that capture; their meaning is not established.
	packet[4] = 0x11;
	packet[5] = 0x11;
	packet[6] = 0x81;
	packet[7] = 0x08;
	packet[8] = static_cast<uint8_t>(timestamp);
	packet[9] = static_cast<uint8_t>(timestamp >> 8);
	packet[10] = static_cast<uint8_t>(timestamp >> 16);
	packet[11] = static_cast<uint8_t>(timestamp >> 24);
	// Reference clock fields: 16000, 16000, 21000, 21000 (little endian).
	// Keep video_format zero and retain the live per-frame timestamp above.
	for (size_t offset : {size_t{12}, size_t{16}})
	{
		packet[offset] = 0x80;
		packet[offset + 1] = 0x3e;
	}
	for (size_t offset : {size_t{20}, size_t{24}})
	{
		packet[offset] = 0x08;
		packet[offset + 1] = 0x52;
	}
	return packet;
}

std::vector<uint8_t> BuildVideoPacket(std::span<const uint8_t> payload, uint16_t sequence,
	uint32_t timestamp, bool init, bool frame_begin, bool chunk_end, bool frame_end, bool idr,
	bool reference_options = false)
{
	std::vector<uint8_t> packet(16 + payload.size());
	packet[0] = static_cast<uint8_t>(0xf0 | ((sequence >> 8) & 3));
	packet[1] = static_cast<uint8_t>(sequence);
	packet[2] = static_cast<uint8_t>(0x08 | (init ? 0x80 : 0) | (frame_begin ? 0x40 : 0) |
		(chunk_end ? 0x20 : 0) | (frame_end ? 0x10 : 0) | ((payload.size() >> 8) & 7));
	packet[3] = static_cast<uint8_t>(payload.size());
	packet[4] = static_cast<uint8_t>(timestamp >> 24);
	packet[5] = static_cast<uint8_t>(timestamp >> 16);
	packet[6] = static_cast<uint8_t>(timestamp >> 8);
	packet[7] = static_cast<uint8_t>(timestamp);
	packet[8] = 0x83; // force decoding
	packet[9] = 0x85;
	packet[10] = 6; // macroblock rows per chunk
	size_t option = 11;
	if (idr)
		packet[option++] = 0x80;
	packet[option++] = 0x82;
	packet[option] = 0; // 59.94 Hz
	if (reference_options)
	{
		// Same options, in the order observed in authenticated console traffic.
		std::fill(packet.begin() + 8, packet.begin() + 16, 0);
		option = 8;
		if (idr)
			packet[option++] = 0x80;
		packet[option++] = 0x82;
		packet[option++] = 0;
		packet[option++] = 0x83;
		packet[option++] = 0x85;
		packet[option] = 6;
	}
	std::copy(payload.begin(), payload.end(), packet.begin() + 16);
	return packet;
}

void StampVideoFrame(uint32_t timestamp, std::vector<uint8_t>& format,
	std::vector<std::vector<uint8_t>>& packets)
{
	for (unsigned byte = 0; byte < 4; ++byte)
	{
		format[8 + byte] = static_cast<uint8_t>(timestamp >> (8 * byte));
		for (auto& packet : packets)
			packet[4 + byte] = static_cast<uint8_t>(timestamp >> (8 * (3 - byte)));
	}
}

std::vector<uint8_t> ReconstructIdr(const std::vector<std::vector<uint8_t>>& chunks)
{
	static constexpr std::array<uint8_t, 4> kStartCode{0x00, 0x00, 0x00, 0x01};
	static constexpr std::array<uint8_t, 11> kGamepadSps{
		0x67, 0x64, 0x00, 0x20, 0xac, 0x2b, 0x40, 0x6c, 0x1e, 0xf3, 0x68,
	};
	static constexpr std::array<uint8_t, 5> kGamepadPps{0x68, 0xee, 0x06, 0x0c, 0xe8};
	static constexpr std::array<uint8_t, 4> kIdrSliceHeader{0x25, 0xb8, 0x04, 0xff};

	std::vector<uint8_t> result;
	auto append = [&result](auto bytes) { result.insert(result.end(), bytes.begin(), bytes.end()); };
	append(kStartCode);
	append(kGamepadSps);
	append(kStartCode);
	append(kGamepadPps);
	append(kStartCode);
	append(kIdrSliceHeader);

	// Emulation-prevention bytes are inserted over the complete slice, including
	// chunk boundaries.  The transmitted DRH chunks themselves remain unescaped.
	int zero_count = 0;
	for (const auto& chunk : chunks)
	{
		for (const uint8_t byte : chunk)
		{
			if (zero_count >= 2 && byte <= 3)
			{
				result.push_back(3);
				zero_count = 0;
			}
			result.push_back(byte);
			zero_count = byte == 0 ? zero_count + 1 : 0;
		}
	}
	return result;
}

bool WriteBinaryFile(const std::filesystem::path& path, std::span<const uint8_t> bytes)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	return output.good();
}
}

class MediaStreamer::VideoEncoder
{
public:
	struct Chunk
	{
		std::vector<uint8_t> bytes;
		int nal_type = 0;
		int reference_priority = 0;
		int first_macroblock = 0;
		int last_macroblock = 0;
	};

	VideoEncoder()
	{
		x264_param_t parameters{};
		x264_param_default_preset(&parameters, "slow", "zerolatency");
		// Optional lower-cost search; retain all DRH bitstream constraints below.
		const char* fast_encode = std::getenv("DRCD_FAST_ENCODE");
		if (fast_encode && std::strcmp(fast_encode, "1") == 0)
		{
			parameters.analyse.i_me_method = X264_ME_DIA;
			parameters.analyse.i_subpel_refine = 2;
			parameters.analyse.i_trellis = 0;
		}
		parameters.i_width = kWidth;
		parameters.i_height = kHeight;
		parameters.i_csp = X264_CSP_I420;
		parameters.i_fps_num = kVideoFpsNumerator;
		parameters.i_fps_den = kVideoFpsDenominator;
		parameters.b_vfr_input = 0;
		parameters.analyse.inter &= ~X264_ANALYSE_PSUB16x16;
		parameters.i_keyint_min = 10;
		parameters.i_keyint_max = 30;
		parameters.i_scenecut_threshold = -1;
		parameters.b_cabac = 1;
		parameters.b_interlaced = 0;
		parameters.i_bframe = 0;
		parameters.i_bframe_pyramid = 0;
		parameters.i_frame_reference = 1;
		parameters.b_constrained_intra = 1;
		parameters.b_intra_refresh = DefaultEnabled("DRCD_INTRA_REFRESH") ? 1 : 0;
		parameters.analyse.i_weighted_pred = 0;
		parameters.analyse.b_weighted_bipred = 0;
		parameters.analyse.b_transform_8x8 = 0;
		parameters.analyse.i_chroma_qp_offset = 0;
		parameters.rc.i_rc_method = X264_RC_CQP;
		parameters.rc.i_qp_constant = parameters.rc.i_qp_min = parameters.rc.i_qp_max = VideoQuantizer();
		parameters.rc.f_ip_factor = 1.0f;
		parameters.b_repeat_headers = 0;
		parameters.b_aud = 0;
		parameters.b_drh_mode = 1;
		parameters.i_threads = 1;
		parameters.b_sliced_threads = 0;
		parameters.i_slice_count = 1;
		parameters.nalu_process = ProcessNal;
		parameters.i_log_level = X264_LOG_WARNING;
		x264_param_apply_profile(&parameters, "main");
		m_encoder = x264_encoder_open(&parameters);
	}

	~VideoEncoder() { if (m_encoder) x264_encoder_close(m_encoder); }
	bool valid() const { return m_encoder != nullptr; }

	std::vector<Chunk> encode(std::vector<uint8_t>& frame, bool request_idr,
		bool& encoded_idr, std::string& error)
	{
		m_chunks = {};
		m_chunk_present = {};
		m_chunk_payloads = {};
		m_chunk_sizes = {};
		m_callback_error.clear();
		x264_picture_t input{};
		x264_picture_init(&input);
		input.opaque = this;
		input.img.i_csp = X264_CSP_I420;
		input.img.i_plane = 3;
		input.img.i_stride[0] = kWidth;
		input.img.i_stride[1] = kWidth / 2;
		input.img.i_stride[2] = kWidth / 2;
		input.img.plane[0] = frame.data();
		input.img.plane[1] = frame.data() + kWidth * kHeight;
		input.img.plane[2] = input.img.plane[1] + kWidth * kHeight / 4;
		input.i_type = request_idr ? X264_TYPE_IDR : X264_TYPE_P;
		x264_nal_t* nals = nullptr;
		int count = 0;
		x264_picture_t output{};
		if (x264_encoder_encode(m_encoder, &nals, &count, &input, &output) < 0)
		{
			error = "x264_encoder_encode failed";
			return {};
		}
		if (!m_callback_error.empty())
		{
			error = m_callback_error;
			return {};
		}
		if (std::count(m_chunk_present.begin(), m_chunk_present.end(), true) != kChunksPerFrame)
		{
			error = "DRH encoder returned " +
				std::to_string(std::count(m_chunk_present.begin(), m_chunk_present.end(), true)) +
				" chunks (expected 5)";
			return {};
		}
		// CABAC may report bytes which are still outstanding at callback time;
		// their final value is resolved as later macroblocks are encoded.  Keep
		// the x264 pointers in the callback and copy only after the frame has
		// completely finished, as the original libdrc encoder does.
		for (size_t index = 0; index < kChunksPerFrame; ++index)
			m_chunks[index].bytes.assign(m_chunk_payloads[index],
				m_chunk_payloads[index] + m_chunk_sizes[index]);

		encoded_idr = std::all_of(m_chunks.begin(), m_chunks.end(), [](const Chunk& chunk) {
			return chunk.nal_type == NAL_SLICE_IDR &&
				chunk.reference_priority != NAL_PRIORITY_DISPOSABLE;
		});
		if (request_idr && !encoded_idr)
		{
			error = "x264 did not honor the requested IDR frame";
			return {};
		}
		return {m_chunks.begin(), m_chunks.end()};
	}

private:
	static void ProcessNal(x264_t*, x264_nal_t* nal, void* opaque)
	{
		if (nal->i_type == NAL_SEI)
			return;
		auto& self = *static_cast<VideoEncoder*>(opaque);
		if (nal->i_payload <= 0 || (nal->i_type != NAL_SLICE && nal->i_type != NAL_SLICE_IDR))
		{
			self.m_callback_error = "unexpected x264 callback: type=" +
				std::to_string(nal->i_type) + " size=" + std::to_string(nal->i_payload);
			return;
		}
		const int chunk_index = nal->i_first_mb / kMacroblocksPerChunk;
		if (chunk_index < 0 || chunk_index >= static_cast<int>(kChunksPerFrame))
		{
			self.m_callback_error = "x264 callback first_mb=" +
				std::to_string(nal->i_first_mb) + " maps outside the five DRH chunks";
			return;
		}
		if (self.m_chunk_present[chunk_index])
		{
			self.m_callback_error = "duplicate x264 callback for DRH chunk " +
				std::to_string(chunk_index);
			return;
		}
		self.m_chunks[chunk_index] = {
			.bytes = {},
			.nal_type = nal->i_type,
			.reference_priority = nal->i_ref_idc,
			.first_macroblock = nal->i_first_mb,
			.last_macroblock = nal->i_last_mb,
		};
		self.m_chunk_payloads[chunk_index] = nal->p_payload;
		self.m_chunk_sizes[chunk_index] = static_cast<size_t>(nal->i_payload);
		self.m_chunk_present[chunk_index] = true;
	}

	x264_t* m_encoder = nullptr;
	std::array<Chunk, kChunksPerFrame> m_chunks{};
	std::array<bool, kChunksPerFrame> m_chunk_present{};
	std::array<const uint8_t*, kChunksPerFrame> m_chunk_payloads{};
	std::array<size_t, kChunksPerFrame> m_chunk_sizes{};
	std::string m_callback_error;
};

MediaStreamer::MediaStreamer(drc_host::RuntimeTransport& transport, std::string path,
	bool black_frames)
	: m_transport(transport), m_path(std::move(path)), m_black_frames(black_frames) {}

MediaStreamer::~MediaStreamer() { stop(); }

bool MediaStreamer::start(std::string& error)
{
	if (m_running.exchange(true))
		return true;
	const char* media_socket = std::getenv("DRCD_CEMU_SOCKET");
	const bool external_media = media_socket && *media_socket;
	if (external_media && (!m_path.empty() || m_black_frames))
	{
		m_running.store(false);
		error = "DRCD_CEMU_SOCKET cannot be combined with file/black test modes";
		return false;
	}
	if (!external_media && !m_black_frames && !std::filesystem::is_regular_file(m_path))
	{
		m_running.store(false);
		error = "media file does not exist: " + m_path;
		return false;
	}
	m_encoder = std::make_unique<VideoEncoder>();
	if (!m_encoder->valid())
	{
		m_encoder.reset();
		m_running.store(false);
		error = "could not initialize DRC x264 encoder";
		return false;
	}
	m_stop.store(false);
	m_audio_packets.store(0);
	if (external_media)
	{
		m_bridge = std::make_unique<drc_ipc::MediaBridge>(true);
		if (!m_bridge->start(media_socket, error))
		{
			m_bridge.reset(); m_encoder.reset(); m_running.store(false);
			return false;
		}
		m_transport.report_status("Cemu media bridge listening at " + std::string(media_socket));
	}
	const char* generated = std::getenv("DRCD_GENERATED_AV");
	m_generated_pattern = m_black_frames && generated && std::strcmp(generated, "1") == 0;
	if (m_generated_pattern)
		m_transport.report_status("Generated A/V: moving grayscale pattern and quiet 440Hz tone; no FFmpeg");
	m_video_thread = std::thread([this] { video_loop(); });
	if (m_bridge)
		m_input_thread = std::thread([this] { input_loop(); });
	if (!m_black_frames || m_generated_pattern || m_bridge)
		m_audio_thread = std::thread([this] { audio_loop(); });
	return true;
}

void MediaStreamer::stop()
{
	m_stop.store(true);
	if (m_video_thread.joinable()) m_video_thread.join();
	if (m_audio_thread.joinable()) m_audio_thread.join();
	if (m_input_thread.joinable()) m_input_thread.join();
	m_bridge.reset();
	m_encoder.reset();
	m_running.store(false);
}

bool MediaStreamer::protocol_self_test(std::string& error)
{
	std::vector<uint8_t> generated_frame(kRawFrameSize);
	GeneratePattern(generated_frame, 0);
	const auto first_pattern = generated_frame;
	GeneratePattern(generated_frame, 1);
	std::array<uint8_t, 1664> generated_pcm{};
	uint32_t tone_phase = 0;
	GenerateTone(generated_pcm, tone_phase);
	if (generated_frame == first_pattern || generated_frame[0] != 16 ||
		generated_frame[4] != 235 || generated_frame[kWidth * kHeight] != 128 ||
		tone_phase != (416 * 440) % 48000 ||
		std::all_of(generated_pcm.begin(), generated_pcm.end(), [](uint8_t value) { return value == 0; }))
	{
		error = "generated pattern/tone validation failed";
		return false;
	}
	for (size_t offset = 0; offset < generated_pcm.size(); offset += 4)
		if (generated_pcm[offset] != generated_pcm[offset + 2] ||
			generated_pcm[offset + 1] != generated_pcm[offset + 3])
		{
			error = "generated tone stereo validation failed";
			return false;
		}
	VideoEncoder encoder;
	if (!encoder.valid())
	{
		error = "could not initialize the DRH x264 encoder";
		return false;
	}

	std::vector<uint8_t> frame(kRawFrameSize, 16);
	std::fill(frame.begin() + kWidth * kHeight, frame.end(), 128);
	bool encoded_idr = false;
	auto chunks = encoder.encode(frame, true, encoded_idr, error);
	if (chunks.size() != kChunksPerFrame || !encoded_idr)
		return false;
	for (size_t index = 0; index < chunks.size(); ++index)
	{
		if (chunks[index].bytes.empty() ||
			chunks[index].first_macroblock / kMacroblocksPerChunk != static_cast<int>(index))
		{
			error = "DRH chunk ordering or payload validation failed at chunk " +
				std::to_string(index);
			return false;
		}
	}

	const auto format = BuildVideoFormatPacket(0x12345678);
	const std::vector<uint8_t> expected_format{
		0x04, 0, 0, 0, 0x11, 0x11, 0x81, 0x08,
		0x78, 0x56, 0x34, 0x12, 0x80, 0x3e, 0, 0,
		0x80, 0x3e, 0, 0, 0x08, 0x52, 0, 0,
		0x08, 0x52, 0, 0, 0, 0, 0, 0};
	if (format != expected_format)
	{
		error = "video-format packet layout validation failed";
		return false;
	}

	const auto packet = BuildVideoPacket(chunks[0].bytes, 0x321, 0x12345678,
		true, true, true, false, true);
	if (packet.size() != chunks[0].bytes.size() + 16 || packet[0] != 0xf3 ||
		packet[1] != 0x21 || (packet[2] & 0xe8) != 0xe8 || packet[4] != 0x12 ||
		packet[5] != 0x34 || packet[6] != 0x56 || packet[7] != 0x78 ||
		packet[8] != 0x83 || packet[9] != 0x85 || packet[10] != 6 ||
		packet[11] != 0x80 || packet[12] != 0x82 || packet[13] != 0)
	{
		error = "VSTRM packet layout validation failed";
		return false;
	}
	for (const bool test_idr : {false, true})
	{
		const auto baseline = BuildVideoPacket(chunks[0].bytes, 0x321, 0x12345678,
			true, true, true, false, test_idr);
		const auto reference = BuildVideoPacket(chunks[0].bytes, 0x321, 0x12345678,
			true, true, true, false, test_idr, true);
		const std::array<uint8_t, 8> expected = test_idr
			? std::array<uint8_t, 8>{0x80, 0x82, 0, 0x83, 0x85, 6, 0, 0}
			: std::array<uint8_t, 8>{0x82, 0, 0x83, 0x85, 6, 0, 0, 0};
		if (reference.size() != baseline.size() ||
			!std::equal(expected.begin(), expected.end(), reference.begin() + 8) ||
			!std::equal(baseline.begin(), baseline.begin() + 8, reference.begin()) ||
			!std::equal(baseline.begin() + 16, baseline.end(), reference.begin() + 16))
		{
			error = "reference VSTRM option-order validation failed";
			return false;
		}
	}
	const std::vector<uint8_t> pcm(kAudioSamplesPerPacket * sizeof(int16_t));
	// Restamping must touch only timestamps, including across unsigned TSF wrap.
	for (const uint32_t stamp : {0x12345678u, uint32_t(1000u - 6250u)})
	{
		auto stamped_format = format;
		std::vector<std::vector<uint8_t>> stamped_packets{packet, packet};
		StampVideoFrame(stamp, stamped_format, stamped_packets);
		if (stamped_format != BuildVideoFormatPacket(stamp))
		{
			error = "send-time format timestamp mismatch";
			return false;
		}
		for (auto stamped : stamped_packets)
		{
			for (unsigned byte = 0; byte < 4; ++byte)
			{
				if (stamped[4 + byte] != static_cast<uint8_t>(stamp >> (8 * (3 - byte))))
				{
					error = "send-time video timestamp mismatch";
					return false;
				}
				stamped[4 + byte] = packet[4 + byte];
			}
			if (stamped != packet)
			{
				error = "send-time stamping changed video payload or flags";
				return false;
			}
		}
	}
	const auto audio = BuildAudioPacket(pcm, 0x321, 0x12345678);
	if (audio.size() != 1672 || audio[0] != 0x23 || audio[1] != 0x21 ||
		audio[2] != 0x06 || audio[3] != 0x80 || audio[4] != 0x78 ||
		audio[5] != 0x56 || audio[6] != 0x34 || audio[7] != 0x12)
	{
		error = "ASTRM PCM packet layout validation failed";
		return false;
	}

	std::vector<std::vector<uint8_t>> chunk_bytes;
	for (const auto& chunk : chunks)
		chunk_bytes.push_back(chunk.bytes);
	const auto annex_b = ReconstructIdr(chunk_bytes);
	if (annex_b.size() <= 32)
	{
		error = "reconstructed Annex-B IDR is unexpectedly short";
		return false;
	}
	if (const char* dump_path = std::getenv("DRCD_MEDIA_SELF_TEST_DUMP"); dump_path && *dump_path)
	{
		if (!WriteBinaryFile(dump_path, annex_b))
		{
			error = "could not write DRH self-test artifact to " + std::string(dump_path);
			return false;
		}
	}
	return true;
}

void MediaStreamer::video_loop()
{
	FILE* pipe = nullptr;
	if (!m_black_frames && !m_bridge)
	{
		const std::string command = "ffmpeg -nostdin -loglevel error -stream_loop -1 -i " +
			ShellQuote(m_path) +
			" -an -vf scale=864:480,fps=60000/1001 -pix_fmt yuv420p -f rawvideo pipe:1";
		pipe = ::popen(command.c_str(), "r");
		if (!pipe) return;
	}
	std::vector<uint8_t> frame(kRawFrameSize);
	if (m_black_frames || m_bridge)
	{
		std::fill(frame.begin(), frame.begin() + kWidth * kHeight, 16);
		std::fill(frame.begin() + kWidth * kHeight, frame.end(), 128);
	}
	uint16_t sequence = 0;
	uint64_t frame_number = 0;
	auto next_frame = std::chrono::steady_clock::now();
	auto next_status = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	bool initialized = false;
	bool force_idr = true;
	bool artifact_dumped = false;
	bool external_active = false;
	bool external_connected = false;
	uint64_t encode_failures = 0;
	uint64_t interval_encode_us = 0;
	uint64_t interval_encode_max_us = 0;
	uint64_t interval_encoded_frames = 0;
	uint64_t interval_idr_frames = 0;
	uint64_t interval_idr_us = 0;
	uint64_t interval_predicted_frames = 0;
	uint64_t interval_predicted_us = 0;
	uint64_t interval_over_budget = 0;
	uint64_t interval_resync_events = 0;
	const char* fast_encode = std::getenv("DRCD_FAST_ENCODE");
	const char* option_order = std::getenv("DRCD_REFERENCE_VIDEO_OPTIONS");
	const bool send_time_video = DefaultEnabled("DRCD_SEND_TIME_VIDEO");
	const bool recovery_init = DefaultEnabled("DRCD_IDR_INIT");
	const bool chunk_pacing = DefaultEnabled("DRCD_CHUNK_PACING");
	m_transport.report_status(chunk_pacing
		? "Video chunk pacing: 0/3/6/9/11ms, packets within each chunk contiguous"
		: "Video chunk pacing disabled: whole-frame burst");
	const char* recovery_pause_option = std::getenv("DRCD_IDR_PAUSE");
	const bool recovery_pause = recovery_pause_option && std::strcmp(recovery_pause_option, "1") == 0;
	m_transport.report_status(recovery_pause
		? "Video recovery experiment: next frame scheduled 33.366ms after IDR send"
		: "Video recovery spacing: baseline frame interval");
	m_transport.report_status(recovery_init
		? "Video recovery experiment: init bit on every IDR packet"
		: "Video recovery: baseline init bit on first session frame only");
	m_transport.report_status(send_time_video
		? "Video timestamp experiment: after pacing sleep, AP clock minus 6250us; PCM unchanged"
		: "Video timestamp: baseline before encoding and pacing sleep");
	m_transport.report_status("Video quantizer: QP=" + std::to_string(VideoQuantizer()));
	const bool reference_options = option_order && std::strcmp(option_order, "1") == 0;
	m_transport.report_status(reference_options
		? "Video options: reference console order experiment"
		: "Video options: baseline order");
	m_transport.report_status(fast_encode && std::strcmp(fast_encode, "1") == 0
		? "Video encoder: fast search experiment (dia/subme2/trellis0)"
		: "Video encoder: baseline slow search");
	m_transport.report_status(DefaultEnabled("DRCD_INTRA_REFRESH")
		? "Video cyclic intra-refresh: enabled (baseline)"
		: "Video cyclic intra-refresh: disabled (compatibility test); requested IDRs retained");
	SerialVideoSender sender;
	m_transport.report_status("Video pipeline: independent serial sender; at most one next frame encoding; no frame drops");
	while (!m_stop.load() && (m_black_frames || m_bridge || ReadExact(pipe, frame)))
	{
		if (m_bridge)
		{
			const bool connected = m_bridge->connected();
			if (connected != external_connected)
			{
				external_connected = connected;
				m_transport.report_status(connected ? "Cemu IPC client connected" : "Cemu IPC client disconnected");
			}
			bool active = false;
			m_bridge->read_video(frame, active);
			if (active != external_active)
			{
				force_idr = true; external_active = active;
				m_transport.report_status(active ? "Cemu source: game" : "Cemu source: idle logo");
			}
		}
		if (m_generated_pattern)
			GeneratePattern(frame, frame_number);
		const bool resync_requested = m_transport.consume_video_resync_event();
		interval_resync_events += resync_requested;
		force_idr = resync_requested || force_idr;
		// libdrc timestamps the input before encoding, then transmits the
		// completed frame on the following frame boundary.
		uint32_t timestamp = m_transport.timestamp_us();
		bool idr = false;
		std::string error;
		const auto encode_started = std::chrono::steady_clock::now();
		auto chunks = m_encoder->encode(frame, force_idr, idr, error);
		const uint64_t encode_us = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - encode_started).count();
		interval_encode_us += encode_us;
		interval_encode_max_us = std::max(interval_encode_max_us, encode_us);
		++interval_encoded_frames;
		interval_over_budget += encode_us > static_cast<uint64_t>(kVideoFramePeriod.count());
		if (chunks.empty())
		{
			++encode_failures;
			if (encode_failures <= 3 || encode_failures % 100 == 0)
				m_transport.report_status("Media encoder rejected frame: " + error);
			force_idr = true;
			std::this_thread::sleep_for(kVideoFramePeriod);
			continue;
		}
		if (idr)
		{
			++interval_idr_frames;
			interval_idr_us += encode_us;
		}
		else
		{
			++interval_predicted_frames;
			interval_predicted_us += encode_us;
		}
		auto format = BuildVideoFormatPacket(timestamp);
		std::vector<std::vector<uint8_t>> frame_packets;
		for (size_t chunk_index = 0; chunk_index < chunks.size(); ++chunk_index)
		{
			auto remaining = std::span<const uint8_t>(chunks[chunk_index].bytes);
			bool first_packet = true;
			while (!remaining.empty())
			{
				const size_t count = std::min(kMaxVideoPayload, remaining.size());
				const bool last_packet = count == remaining.size();
				const auto packet = BuildVideoPacket(remaining.first(count), sequence++, timestamp,
					VideoInitFlag(initialized, idr, recovery_init), chunk_index == 0 && first_packet, last_packet,
					chunk_index + 1 == chunks.size() && last_packet, idr, reference_options);
				sequence &= 0x3ff;
				frame_packets.push_back(packet);
				remaining = remaining.subspan(count);
				first_packet = false;
			}
		}

		const bool dump_frame = !artifact_dumped && idr;
		artifact_dumped |= dump_frame;
		if (!sender.Submit([&, chunks = std::move(chunks), format = std::move(format),
			frame_packets = std::move(frame_packets), timestamp, idr, frame_number,
			dump_frame, error = std::move(error)]() mutable {
		// The sender exclusively owns the pacing deadline and packet buffers.
		// The producer can encode the next frame during this frame's waits.
		// Preserve the frame boundary, then pace chunks, not individual packets.
		// Rebase after an overrun without catching up.
		next_frame = std::max(next_frame + kVideoFramePeriod,
			std::chrono::steady_clock::now());
		std::this_thread::sleep_until(next_frame);
		if (m_stop.load())
			return;
		if (send_time_video)
		{
			// The reference console's beacon-aligned video age is about 6.27ms.
			// Stamp AFTER both encoding and sleep: a slow IDR must not acquire
			// extra timestamp age from either. Do not alter cadence or PCM.
			timestamp = m_transport.timestamp_us() - 6250u;
			StampVideoFrame(timestamp, format, frame_packets);
		}
		const auto frame_send_started = std::chrono::steady_clock::now();
		bool sends_ok = m_transport.send(drc_host::RuntimeChannel::Audio, format, error);
		size_t sending_chunk = 0;
		for (size_t packet_index = 0; packet_index < frame_packets.size(); ++packet_index)
		{
			if (chunk_pacing && (packet_index == 0 || (frame_packets[packet_index - 1][2] & 0x20)))
			{
				std::this_thread::sleep_until(frame_send_started + kChunkSendOffsets.at(sending_chunk++));
				if (m_stop.load()) break;
			}
			sends_ok = m_transport.send(drc_host::RuntimeChannel::Video,
				frame_packets[packet_index], error) && sends_ok;
		}
		if (recovery_pause && idr)
		{
			// The next loop adds one more frame period before sending. Anchor
			// this extra interval to the actual IDR send, not an expired deadline.
			// No frame is encoded then discarded: preserve the reference chain.
			// Audio and input workers remain independent of video pacing.
			next_frame = frame_send_started + kVideoFramePeriod;
		}
		if (dump_frame)
		{
			const char* configured_path = std::getenv("DRCD_MEDIA_DUMP_DIR");
			const std::filesystem::path dump_dir = configured_path && *configured_path
				? configured_path : "/tmp/drcd-media-dump";
			std::error_code filesystem_error;
			std::filesystem::create_directories(dump_dir, filesystem_error);
			bool dump_ok = !filesystem_error && WriteBinaryFile(dump_dir / "video-format.bin", format);
			std::vector<std::vector<uint8_t>> chunk_bytes;
			for (size_t index = 0; index < chunks.size(); ++index)
			{
				chunk_bytes.push_back(chunks[index].bytes);
				dump_ok = WriteBinaryFile(dump_dir /
					("chunk-" + std::to_string(index) + ".bin"), chunks[index].bytes) && dump_ok;
			}
			const auto annex_b = ReconstructIdr(chunk_bytes);
			dump_ok = WriteBinaryFile(dump_dir / "first-idr.h264", annex_b) && dump_ok;
			std::vector<uint8_t> datagrams;
			for (const auto& packet : frame_packets)
			{
				datagrams.push_back(static_cast<uint8_t>(packet.size() >> 8));
				datagrams.push_back(static_cast<uint8_t>(packet.size()));
				datagrams.insert(datagrams.end(), packet.begin(), packet.end());
			}
			dump_ok = WriteBinaryFile(dump_dir / "vstrm.datagrams", datagrams) && dump_ok;

			std::ofstream manifest(dump_dir / "manifest.txt", std::ios::trunc);
			manifest << "timestamp=" << timestamp << '\n'
				<< "chunks=" << chunks.size() << '\n'
				<< "datagrams=" << frame_packets.size() << '\n';
			for (size_t index = 0; index < chunks.size(); ++index)
				manifest << "chunk[" << index << "] size=" << chunks[index].bytes.size()
					<< " nal_type=" << chunks[index].nal_type
					<< " ref_idc=" << chunks[index].reference_priority
					<< " first_mb=" << chunks[index].first_macroblock
					<< " last_mb=" << chunks[index].last_macroblock << '\n';
			dump_ok = manifest.good() && dump_ok;
			m_transport.report_status(dump_ok
				? "Captured first valid five-chunk IDR at " + dump_dir.string()
				: "Failed to capture complete first-IDR diagnostics at " + dump_dir.string());
		}
		if (frame_number == 0)
		{
			std::ostringstream details;
			details << "Media first frame: " << chunks.size() << " DRH chunks, "
				<< frame_packets.size() << " VSTRM packets, timestamp=" << timestamp;
			for (size_t index = 0; index < chunks.size(); ++index)
				details << " c" << index << '=' << chunks[index].bytes.size()
					<< "B/mb" << chunks[index].first_macroblock << '-' << chunks[index].last_macroblock;
			details << ", sends " << (sends_ok ? "OK" : "FAILED: " + error);
			m_transport.report_status(details.str());
		}
		}))
		{
			m_transport.report_status("Video sender failed; stopping media to preserve reference order");
			m_stop.store(true);
			break;
		}
		initialized = true;
		force_idr = false;
		++frame_number;
		if (std::chrono::steady_clock::now() >= next_status)
		{
			m_transport.report_status("Video encode timing: frames=" + std::to_string(interval_encoded_frames) +
				" avg_us=" + std::to_string(interval_encode_us / std::max<uint64_t>(1, interval_encoded_frames)) +
				" max_us=" + std::to_string(interval_encode_max_us) +
				" over_budget=" + std::to_string(interval_over_budget) +
				" idr_frames=" + std::to_string(interval_idr_frames) +
				" idr_avg_us=" + std::to_string(interval_idr_us / std::max<uint64_t>(1, interval_idr_frames)) +
				" p_frames=" + std::to_string(interval_predicted_frames) +
				" p_avg_us=" + std::to_string(interval_predicted_us / std::max<uint64_t>(1, interval_predicted_frames)) +
				" resync_events=" + std::to_string(interval_resync_events));
			interval_encode_us = interval_encode_max_us = interval_encoded_frames = 0;
			interval_idr_frames = interval_idr_us = interval_predicted_frames = interval_predicted_us = 0;
			interval_over_budget = interval_resync_events = 0;
			std::cout << "Media stream active: " << frame_number << " video frames, "
				<< m_audio_packets.load() << " audio packets" << std::endl;
			const auto stats = m_transport.stats();
			m_transport.report_status("Media UDP: video=" + std::to_string(stats.video_packets_sent) +
				" audio=" + std::to_string(stats.audio_packets_sent) +
				" command=" + std::to_string(stats.command_packets_sent) +
				" uvc_replies=" + std::to_string(stats.uvc_uac_replies) +
				" command_retries=" + std::to_string(stats.command_retries) +
				" command_timeouts=" + std::to_string(stats.command_timeouts) +
				" pad_streaming=" + (stats.waiting_for_streaming ? "waiting" : "active") +
				" errors=" + std::to_string(stats.send_errors) +
				" resync=" + std::to_string(stats.video_resync_requests));
			next_status += std::chrono::seconds(5);
		}
	}
	if (!sender.Finish())
	{
		m_transport.report_status("Video sender failed while finishing media");
		m_stop.store(true);
	}
	if (pipe)
		::pclose(pipe);
}

void MediaStreamer::input_loop()
{
	// Input must not wait for the next encoded frame (IDRs can take >60ms).
	m_transport.report_status("Cemu input forwarding: independent 1ms worker");
	while (!m_stop.load())
	{
		// Bound the batch so shutdown cannot be starved by incoming traffic.
		for (unsigned count = 0; count < 256; ++count)
		{
			auto packet = m_transport.receive();
			if (!packet) break;
			if (packet->channel == drc_host::RuntimeChannel::Input)
				m_bridge->submit_input(packet->payload);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

void MediaStreamer::audio_loop()
{
	FILE* pipe = nullptr;
	if (!m_generated_pattern && !m_bridge)
	{
		const std::string command = "ffmpeg -nostdin -loglevel error -stream_loop -1 -i " + ShellQuote(m_path) +
			" -vn -ac 2 -ar 48000 -f s16le pipe:1";
		pipe = ::popen(command.c_str(), "r");
		if (!pipe) return;
	}
	std::array<uint8_t, kAudioSamplesPerPacket * sizeof(int16_t)> pcm{};
	uint16_t sequence = 0;
	uint32_t tone_phase = 0;
	auto next_packet = std::chrono::steady_clock::now();
	while (!m_stop.load() && (m_generated_pattern || m_bridge || ReadExact(pipe, pcm)))
	{
		if (m_bridge)
			m_bridge->read_pcm(pcm);
		if (m_generated_pattern)
			GenerateTone(pcm, tone_phase);
		// FFmpeg startup and reads may block. Do not replay missed audio slots
		// as a burst; schedule from the first available block after a stall.
		next_packet = std::max(next_packet, std::chrono::steady_clock::now());
		std::this_thread::sleep_until(next_packet);
		const auto packet = BuildAudioPacket(pcm, sequence++, m_transport.timestamp_us());
		sequence &= 0x3ff;
		std::string error;
		(void)m_transport.send(drc_host::RuntimeChannel::Audio, packet, error);
		++m_audio_packets;
		next_packet += kAudioPacketPeriod;
	}
	if (pipe) ::pclose(pipe);
}
}
