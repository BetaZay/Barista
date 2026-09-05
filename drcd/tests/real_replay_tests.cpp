#include "../src/real_replay.h"
#include "drcd/media_streamer.h"
#include "drc_host/runtime_transport.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <iostream>
#include <cstdlib>
#include <array>

// Integration test against a prepared capture; no radio or credentials required.
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto replay = drcd::RealReplay::Load(argv[1]);
    unsetenv("DRCD_CEMU_SOCKET"); unsetenv("DRCD_AP_TSF_CLOCK");
    setenv("DRCD_REAL_REPLAY", argv[1], 1);
    std::array<int,2> sockets{};
    for (size_t i=0; i<2; ++i) {
        sockets[i] = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        int buffer = 4*1024*1024;
        setsockopt(sockets[i], SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
        sockaddr_in address{}; address.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.2", &address.sin_addr);
        address.sin_port = htons(50120+i);
        if (bind(sockets[i], reinterpret_cast<sockaddr*>(&address), sizeof(address))) return 1;
    }
    drc_host::RuntimeTransport transport;
    std::string error;
    if (!transport.start({.console_address="127.0.0.1", .gamepad_address="127.0.0.2"}, error)) return 1;
    drcd::MediaStreamer streamer(transport, "", true);
    if (!streamer.start(error)) { std::cerr << error; return 1; }
    std::array<std::vector<std::vector<uint8_t>>,2> actual;
    using namespace std::chrono;
    const auto end = steady_clock::now() + seconds(2);
    while (steady_clock::now() < end) {
        for (size_t i=0; i<2; ++i) {
            uint8_t data[2048]; ssize_t n;
            while ((n=recv(sockets[i], data, sizeof(data), 0)) > 0)
                actual[i].emplace_back(data, data+n);
        }
        std::this_thread::sleep_for(microseconds(250));
    }
    streamer.stop(); transport.stop(); for (int fd : sockets) close(fd);
    std::array<size_t,2> index{};
    uint16_t video_seq=0, pcm_seq=0;
    uint32_t common_delta=0; bool have_delta=false;
    for (const auto& record : replay.packets) {
        const size_t channel = record.kind == 0 ? 0 : 1;
        if (index[channel] >= actual[channel].size()) continue;
        auto expected = record.data;
        const auto& received = actual[channel][index[channel]++];
        if (expected.size() != received.size()) return 1;
        const size_t pos = record.kind == 1 ? 8 : 4;
        auto stamp = [&](const auto& p) -> uint32_t {
            return record.kind == 0 ? (uint32_t(p[4])<<24) | (uint32_t(p[5])<<16) |
                (uint32_t(p[6])<<8) | p[7] : drcd::RealReplay::LE(p.data()+pos);
        };
        const uint32_t delta = stamp(received) - stamp(expected);
        if (!have_delta) { common_delta=delta; have_delta=true; }
        if (delta != common_delta) { std::cerr << "timestamp relationship changed"; return 1; }
        for (size_t j=0; j<4; ++j) expected[pos+j]=received[pos+j];
        if (record.kind != 1) {
            auto& seq = record.kind == 0 ? video_seq : pcm_seq;
            expected[0]=(expected[0]&0xfc) | ((seq>>8)&3); expected[1]=seq&255; seq=(seq+1)&1023;
        }
        if (expected != received) { std::cerr << "replay packet changed"; return 1; }
    }
    std::cout << "Verified original payloads/flags/boundaries and common clock translation: video="
        << index[0] << " audio/format=" << index[1] << '\n';
    return index[0] > 100 && index[1] > 100 ? 0 : 1;
}
