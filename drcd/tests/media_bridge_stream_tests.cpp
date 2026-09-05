#include "drcd/media_streamer.h"
#include "drc_host/runtime_transport.h"
#include "drc_ipc/media_bridge.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <thread>

int main()
{
    char directory[] = "/tmp/drc-stream-test-XXXXXX";
    if (!mkdtemp(directory)) return 1;
    const std::string path = std::string(directory) + "/media.sock";
    setenv("DRCD_CEMU_SOCKET", path.c_str(), 1);
    setenv("DRCD_MEDIA_DUMP_DIR", directory, 1);
    int video = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    int audio = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    sockaddr_in address{}; address.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.2", &address.sin_addr);
    address.sin_port = htons(50120);
    if (bind(video, reinterpret_cast<sockaddr*>(&address), sizeof(address))) return 1;
    address.sin_port = htons(50121);
    if (bind(audio, reinterpret_cast<sockaddr*>(&address), sizeof(address))) return 1;
    drc_host::RuntimeTransport transport;
    std::string error;
    if (!transport.start({.console_address="127.0.0.1", .gamepad_address="127.0.0.2"}, error))
    { std::cerr << error; return 1; }
    drcd::MediaStreamer media(transport, "");
    if (!media.start(error)) { std::cerr << error; return 1; }
    drc_ipc::MediaBridge client(false);
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
    const char* pause_option = std::getenv("DRCD_IDR_PAUSE");
    const bool pause_test = pause_option && std::strcmp(pause_option, "1") == 0;
    std::vector<int64_t> after_idr_gaps, after_p_gaps;
    auto previous_frame_time = Clock::time_point{};
    bool previous_idr = false;
    unsigned recovery_idrs = 0;
    bool recovery_flags_ok = true;
    std::vector<uint32_t> video_ages;
    bool activated = false, input_received = false, audible_pcm = false;
    std::array<uint8_t, 128> input{}; input[2] = 0x80;
    sockaddr_in console{}; console.sin_family = AF_INET; console.sin_port = htons(50022);
    inet_pton(AF_INET, "127.0.0.1", &console.sin_addr);
    std::array<int16_t, 832> samples{}; samples.fill(1234);
    auto next_audio = Clock::now();
    auto next_resync = Clock::now() + std::chrono::milliseconds(500);
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
        if ((recovery_test || pause_test) && Clock::now() >= next_resync)
        {
            auto message_address = console;
            message_address.sin_port = htons(50010);
            const uint8_t request[]{1,0,0,0};
            sendto(video, request, sizeof(request), 0,
                reinterpret_cast<sockaddr*>(&message_address), sizeof(message_address));
            next_resync = Clock::now() + std::chrono::milliseconds(500);
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
                    ++frames;
                    const uint32_t stamp = (uint32_t(packet[4]) << 24) |
                        (uint32_t(packet[5]) << 16) | (uint32_t(packet[6]) << 8) | packet[7];
                    video_ages.push_back(transport.timestamp_us() - stamp);
                }
                if (fd == audio && n == 1672 && !(packet[0] & 4))
                {
                    ++pcm_packets;
                    audible_pcm |= packet[8] == 0xd2 && packet[9] == 4;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    client.stop(); media.stop(); transport.stop(); close(video); close(audio);
    const char* send_time = std::getenv("DRCD_SEND_TIME_VIDEO");
    bool timing_ok = true;
    if (!send_time || std::strcmp(send_time, "0") != 0)
    {
        std::sort(video_ages.begin(), video_ages.end());
        const uint32_t median = video_ages.empty() ? 0 : video_ages[video_ages.size() / 2];
        std::cout << "send_time_median_age_us=" << median << '\n';
        // Includes the 1ms receiver polling delay, but must exclude the
        // sender's 16.683ms pacing sleep. Allows moderate scheduler jitter.
        timing_ok = median >= 6250 && median < 12000;
    }
    std::cout << "frames=" << frames << " pcm=" << pcm_packets << " audio_data=" << audible_pcm
              << " input=" << input_received << " artifacts=" << directory << '\n';
    if (recovery_test)
        std::cout << "recovery_idrs=" << recovery_idrs << " packet_flags_ok=" << recovery_flags_ok << '\n';
    bool spacing_ok = true;
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
            pacing_ok &= offsets.size() >= 100 && std::abs(median - expected[i]) < 1800;
        }
    }
    if (pause_test)
    {
        std::sort(after_idr_gaps.begin(), after_idr_gaps.end());
        std::sort(after_p_gaps.begin(), after_p_gaps.end());
        const auto idr_gap = after_idr_gaps.empty() ? 0 : after_idr_gaps[after_idr_gaps.size()/2];
        const auto p_gap = after_p_gaps.empty() ? 0 : after_p_gaps[after_p_gaps.size()/2];
        std::cout << "post_idr_median_us=" << idr_gap << " post_p_median_us=" << p_gap << '\n';
        spacing_ok = after_idr_gaps.size() >= 4 && after_p_gaps.size() >= 100 &&
            idr_gap >= 30000 && idr_gap < 40000 && p_gap >= 14000 && p_gap < 23000;
    }
    return frames >= 140 && pcm_packets >= 300 && input_received && audible_pcm && timing_ok &&
        spacing_ok && pacing_ok && (!recovery_test || (recovery_idrs >= 4 && recovery_flags_ok)) ? 0 : 1;
}
