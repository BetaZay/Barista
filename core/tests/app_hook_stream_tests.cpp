#include "drh/encoder/media_streamer.h"
#include "drh/runtime_transport.h"
#include "api/app_hook.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <map>
#include <thread>

int main()
{
    char directory[] = "/tmp/drc-stream-test-XXXXXX";
    if (!mkdtemp(directory)) return 1;
    const std::string path = std::string(directory) + "/media.sock";
    setenv("BARISTA_MUG_SOCKET", path.c_str(), 1);
    setenv("DRCD_MEDIA_DUMP_DIR", directory, 1);
    int video = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    int audio = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    sockaddr_in address{}; address.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.2", &address.sin_addr);
    address.sin_port = htons(50120);
    if (bind(video, reinterpret_cast<sockaddr*>(&address), sizeof(address))) return 1;
    address.sin_port = htons(50121);
    if (bind(audio, reinterpret_cast<sockaddr*>(&address), sizeof(address))) return 1;
    barista::drh::RuntimeTransport transport;
    std::string error;
    if (!transport.start({.console_address="127.0.0.1", .gamepad_address="127.0.0.2"}, error))
    { std::cerr << error; return 1; }
    barista::drh::MediaStreamer media(transport, "");
    // Exercise the service-to-engine idle surface handoff without changing
    // the codec/packet/timing assertions below. Malformed input fails closed.
    const auto idle_path = std::string(directory) + "/idle.i420";
    setenv("BARISTA_IDLE_I420", idle_path.c_str(), 1);
    if (media.start(error)) { std::cerr << "missing idle file accepted"; return 1; }
    auto write_idle = [&](size_t bytes) {
        std::ofstream file(idle_path, std::ios::binary | std::ios::trunc);
        const std::vector<uint8_t> idle(bytes,128);
        file.write(reinterpret_cast<const char*>(idle.data()), idle.size());
        return bool(file);
    };
    if (!write_idle(1) || media.start(error)) { std::cerr << "short idle file accepted"; return 1; }
    if (!write_idle(barista::api::FrameBytes+1) || media.start(error)) { std::cerr << "oversized idle file accepted"; return 1; }
    if (!write_idle(barista::api::FrameBytes)) return 1;
    if (!media.start(error)) { std::cerr << error; return 1; }
    barista::api::AppHook client(false);
    client.submit_rgb(std::vector<uint8_t>(12, 32), 2, 2, true);
    if (!client.start(path, error)) return 1;
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    unsigned frames = 0, pcm_packets = 0;
    const char* recovery_option = std::getenv("DRCD_IDR_INIT");
    const bool recovery_test = !recovery_option || std::strcmp(recovery_option, "0") != 0;
    const char* pacing_option = std::getenv("DRCD_CHUNK_PACING");
    const bool pacing_test = !pacing_option || std::strcmp(pacing_option, "0") != 0;
    std::array<std::vector<int64_t>, 5> chunk_offsets;
    Clock::time_point chunk_frame_start{};
    size_t receive_chunk = 0;
    bool chunk_start = true;
    bool chunk_timestamps_ok = true;
    bool packet_order_ok = true;
    int previous_video_sequence = -1;
    std::array<uint8_t, 4> frame_timestamp{};
    // Recovery spacing is now unconditional, not an opt-in pause of formats.
    const bool pause_test = true;
    std::vector<int64_t> after_idr_gaps, after_p_gaps;
    auto previous_frame_time = Clock::time_point{};
    bool previous_idr = false;
    unsigned recovery_idrs = 0;
    bool recovery_flags_ok = true;
    std::vector<uint32_t> video_ages;
    std::vector<uint32_t> format_ages;
    std::map<uint32_t, Clock::time_point> format_times, video_times;
    std::vector<std::pair<uint32_t, bool>> video_order;
    std::vector<uint32_t> audio_ages;
    const char* all_idr_option = std::getenv("DRCD_ALL_IDR");
    const bool all_idr = all_idr_option && std::strcmp(all_idr_option, "1") == 0;
    unsigned predicted_frames = 0;
    bool activated = false, input_received = false, audible_pcm = false;
    std::array<uint8_t, 128> input{}; input[2] = 0x80;
    sockaddr_in console{}; console.sin_family = AF_INET; console.sin_port = htons(50022);
    inet_pton(AF_INET, "127.0.0.1", &console.sin_addr);
    std::array<int16_t, 832> samples{}; samples.fill(1234);
    auto next_audio = Clock::now();
    auto next_resync = Clock::now() + std::chrono::milliseconds(500);
    auto burst_until = Clock::time_point{};
    auto next_burst = Clock::time_point{};
    while (Clock::now() - started < std::chrono::seconds(3))
    {
        if (client.connected() && !activated)
        {
            client.set_active(true);
            client.submit_rgb(std::vector<uint8_t>(12, 220), 2, 2);
            activated = true;
        }
        if (Clock::now() >= next_audio)
        {
            client.submit_pcm(samples);
            sendto(video, input.data(), input.size(), 0, reinterpret_cast<sockaddr*>(&console), sizeof(console));
            next_audio = Clock::now() + std::chrono::milliseconds(8);
        }
        const bool periodic_request = Clock::now() >= next_resync;
        const bool burst_request = Clock::now() < burst_until && Clock::now() >= next_burst;
        if (periodic_request || burst_request)
        {
            auto message_address = console;
            message_address.sin_port = htons(50010);
            const uint8_t request[]{1,0,0,0};
            sendto(video, request, sizeof(request), 0,
                reinterpret_cast<sockaddr*>(&message_address), sizeof(message_address));
            if (periodic_request) next_resync = Clock::now() + std::chrono::milliseconds(500);
            next_burst = Clock::now() + std::chrono::milliseconds(2);
        }
        std::array<uint8_t, 128> received{};
        input_received |= client.read_input(received) && received[2] == 0x80;
        for (int fd : {video, audio})
        {
            uint8_t packet[2048]; ssize_t n;
            while ((n = recv(fd, packet, sizeof(packet), 0)) > 0)
            {
                if (fd == video && n >= 16)
                {
                    const int sequence = ((packet[0] & 3) << 8) | packet[1];
                    if (previous_video_sequence >= 0)
                        packet_order_ok &= sequence == ((previous_video_sequence + 1) & 1023);
                    previous_video_sequence = sequence;
                    const auto now = Clock::now();
                    if (packet[2] & 0x40)
                    {
                        chunk_frame_start = now;
                        receive_chunk = 0;
                        chunk_start = true;
                        std::copy(packet + 4, packet + 8, frame_timestamp.begin());
                    }
                    chunk_timestamps_ok &= std::equal(frame_timestamp.begin(), frame_timestamp.end(), packet + 4);
                    if (chunk_frame_start != Clock::time_point{} && chunk_start && receive_chunk < 5)
                        chunk_offsets[receive_chunk].push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                            now - chunk_frame_start).count());
                    chunk_start = packet[2] & 0x20;
                    if (chunk_start) ++receive_chunk;
                }
                if (recovery_test && fd == video && n >= 16)
                {
                    const bool idr = std::find(packet + 8, packet + 16, uint8_t{0x80}) != packet + 16;
                    recovery_flags_ok &= bool(packet[2] & 0x80) == idr;
                    if (idr && (packet[2] & 0x40)) ++recovery_idrs;
                }
                if (fd == video && n >= 16 && (packet[2] & 0x40))
                {
                    const auto received_at = Clock::now();
                    if (previous_frame_time != Clock::time_point{})
                    {
                        const auto gap = std::chrono::duration_cast<std::chrono::microseconds>(
                            received_at - previous_frame_time).count();
                        (previous_idr ? after_idr_gaps : after_p_gaps).push_back(gap);
                    }
                    previous_frame_time = received_at;
                    previous_idr = std::find(packet + 8, packet + 16, uint8_t{0x80}) != packet + 16;
                    if (previous_idr && !all_idr)
                        burst_until = received_at + std::chrono::milliseconds(24);
                    predicted_frames += !previous_idr;
                    ++frames;
                    const uint32_t stamp = (uint32_t(packet[4]) << 24) |
                        (uint32_t(packet[5]) << 16) | (uint32_t(packet[6]) << 8) | packet[7];
                    video_ages.push_back(transport.timestamp_us() - stamp);
                    video_times.emplace(stamp, received_at);
                    video_order.emplace_back(stamp, previous_idr);
                }
                if (fd == audio && n == 32 && packet[0] == 4)
                {
                    const uint32_t stamp = uint32_t(packet[8]) | (uint32_t(packet[9]) << 8) |
                        (uint32_t(packet[10]) << 16) | (uint32_t(packet[11]) << 24);
                    format_times.emplace(stamp, Clock::now());
                    format_ages.push_back(transport.timestamp_us() - stamp);
                }
                if (fd == audio && n == 1672 && !(packet[0] & 4))
                {
                    ++pcm_packets;
                    const uint32_t stamp = uint32_t(packet[4]) | (uint32_t(packet[5]) << 8) |
                        (uint32_t(packet[6]) << 16) | (uint32_t(packet[7]) << 24);
                    audio_ages.push_back(transport.timestamp_us() - stamp);
                    audible_pcm |= packet[8] == 0xd2 && packet[9] == 4;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    client.stop(); media.stop(); transport.stop(); close(video); close(audio);
    const char* send_time = std::getenv("DRCD_SEND_TIME_VIDEO");
    bool timing_ok = true;
    // All-IDR diagnostics intentionally spend every other slot on formats only.
    const unsigned minimum_frames = all_idr ? 75 : 140;
    if (!send_time || std::strcmp(send_time, "0") != 0)
    {
        std::sort(video_ages.begin(), video_ages.end());
        const uint32_t median = video_ages.empty() ? 0 : video_ages[video_ages.size() / 2];
        std::cout << "send_time_median_age_us=" << median << '\n';
        // Includes the 1ms receiver polling delay, but must exclude the
        // sender's 16.683ms pacing sleep. Allows moderate scheduler jitter.
        timing_ok = median >= 6250 && median < 12000;
        std::sort(format_ages.begin(), format_ages.end());
        const uint32_t format_median = format_ages.empty() ? 0 : format_ages[format_ages.size()/2];
        std::vector<int64_t> leads;
        for (const auto& [stamp, when] : video_times)
        {
            const auto found = format_times.find(stamp);
            if (found != format_times.end())
                leads.push_back(std::chrono::duration_cast<std::chrono::microseconds>(when - found->second).count());
        }
        std::sort(leads.begin(), leads.end());
        const auto lead_median = leads.empty() ? 0 : leads[leads.size()/2];
        std::cout << "format_median_age_us=" << format_median << " format_video_lead_us=" << lead_median
                  << " matched_timestamps=" << leads.size() << '\n';
        timing_ok &= leads.size() == video_times.size() && leads.size() >= minimum_frames &&
            format_median >= 1250 && format_median < 4500 && lead_median >= 3000 && lead_median < 7500;
    }
    std::cout << "frames=" << frames << " pcm=" << pcm_packets << " audio_data=" << audible_pcm
              << " input=" << input_received << " artifacts=" << directory << '\n';
    if (recovery_test)
        std::cout << "recovery_idrs=" << recovery_idrs << " packet_flags_ok=" << recovery_flags_ok << '\n';
    bool spacing_ok = true;
    const char* reference_audio = std::getenv("DRCD_REFERENCE_AUDIO_TIME");
    const uint32_t expected_audio_age = reference_audio && std::strcmp(reference_audio, "1") == 0 ? 10000u : 0u;
    std::sort(audio_ages.begin(), audio_ages.end());
    const uint32_t audio_median = audio_ages.empty() ? 0 : audio_ages[audio_ages.size()/2];
    std::cout << "pcm_median_age_us=" << audio_median << " predicted_frames=" << predicted_frames << '\n';
    timing_ok &= !audio_ages.empty() && audio_median >= expected_audio_age && audio_median < expected_audio_age + 6000;
    const bool reference_chain_ok = all_idr ? predicted_frames == 0 && recovery_idrs == frames : predicted_frames > 0;
    bool pacing_ok = chunk_timestamps_ok && packet_order_ok;
    if (pacing_test)
    {
        constexpr int64_t expected[]{0, 3000, 6000, 9000, 11000};
        for (size_t i = 0; i < chunk_offsets.size(); ++i)
        {
            auto& offsets = chunk_offsets[i];
            std::sort(offsets.begin(), offsets.end());
            const auto median = offsets.empty() ? -1 : offsets[offsets.size()/2];
            std::cout << "chunk=" << i << " median_offset_us=" << median << '\n';
            pacing_ok &= offsets.size() >= minimum_frames - 5 && std::abs(median - expected[i]) < 1800;
        }
    }
    if (pause_test)
    {
        std::sort(after_idr_gaps.begin(), after_idr_gaps.end());
        std::sort(after_p_gaps.begin(), after_p_gaps.end());
        const auto idr_gap = after_idr_gaps.empty() ? 0 : after_idr_gaps[after_idr_gaps.size()/2];
        const auto p_gap = after_p_gaps.empty() ? 0 : after_p_gaps[after_p_gaps.size()/2];
        std::cout << "post_idr_median_us=" << idr_gap << " post_p_median_us=" << p_gap << '\n';
        spacing_ok = after_idr_gaps.size() >= 4 && idr_gap >= 30000 && idr_gap < 43000;
        if (!all_idr)
            spacing_ok &= after_p_gaps.size() >= 100 && p_gap >= 14000 && p_gap < 23000;
    }
    // For every observed IDR transition verify an actual format-only slot, not
    // merely a longer video gap. Sequence continuity above checks no video drop.
    unsigned recovery_gaps = 0;
    bool recovery_slots_ok = true;
    if (!send_time || std::strcmp(send_time, "0") != 0)
        for (size_t i = 1; i < video_order.size(); ++i)
        {
            const auto [previous_stamp, was_idr] = video_order[i - 1];
            if (!was_idr) continue;
            const auto [stamp, is_idr] = video_order[i];
            unsigned empty_formats = 0;
            for (const auto& [format_stamp, when] : format_times)
                if (uint32_t(format_stamp - previous_stamp) < uint32_t(stamp - previous_stamp) &&
                    format_stamp != previous_stamp && !video_times.contains(format_stamp)) ++empty_formats;
            recovery_slots_ok &= empty_formats >= 1;
            // Source activation may explicitly force a second startup IDR.
            if (!all_idr && i > 4) recovery_slots_ok &= !is_idr;
            ++recovery_gaps;
        }
    std::cout << "format_packets=" << format_times.size() << " recovery_gaps=" << recovery_gaps
              << " recovery_slots_ok=" << recovery_slots_ok << '\n';
    return frames >= minimum_frames && format_times.size() >= 165 && recovery_slots_ok &&
        pcm_packets >= 300 && input_received && audible_pcm && timing_ok &&
        reference_chain_ok && spacing_ok && pacing_ok && (!recovery_test || (recovery_idrs >= 4 && recovery_flags_ok)) ? 0 : 1;
}
