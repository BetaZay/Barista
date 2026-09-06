#include "drc_ipc/app_hook.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void wait_for(F f)
{
    for (int i = 0; i < 400; ++i) { if (f()) return; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    throw std::runtime_error("timeout");
}
int main()
{
    char directory[] = "/tmp/drc-ipc-test-XXXXXX";
    check(mkdtemp(directory), "mkdtemp");
    const auto path = std::string(directory) + "/media.sock";
    try
    {
        std::vector<uint8_t> black(6 * 6 * 3, 0), white(6 * 6 * 3, 255);
        auto converted = drc_ipc::AppHook::rgb_to_i420(black, 6, 6);
        check(converted.size() == drc_ipc::FrameBytes && converted[0] == 16 && converted.back() == 128, "black conversion");
        check(drc_ipc::AppHook::rgb_to_i420({}, 0, 0).empty(), "invalid conversion");
        drc_ipc::AppHook server(true), client(false), duplicate(true);
        std::string error;
        check(server.start(path, error), error.c_str());
        check(!duplicate.start(path, error), "must not replace existing socket");
        bool idleActive = true;
        std::vector<uint8_t> idleFrame(drc_ipc::FrameBytes);
        check(!server.set_idle_frame({converted.data(), 12}), "reject malformed fallback");
        check(server.set_idle_frame(converted), "set server fallback");
        check(server.read_video(idleFrame, idleActive) && !idleActive && idleFrame == converted,
            "server fallback before any connector");
        check(server.set_idle_frame({}), "clear server fallback for legacy behavior");
        client.submit_rgb(black, 6, 6, true);
        check(client.start(path, error), error.c_str());
        wait_for([&] { return server.connected() && client.connected(); });
        bool active = false;
        std::vector<uint8_t> frame(drc_ipc::FrameBytes);
        wait_for([&] { return server.read_video(frame, active) && frame[0] == 16 && !active; });
        client.set_active(true);
        wait_for([&] { server.read_video(frame, active); return active; });
        client.submit_rgb(white, 6, 6);
        wait_for([&] { return server.read_video(frame, active) && frame[0] == 235 && active; });
        std::array<int16_t, 4> pcm{123, -456, 789, -1000}; client.submit_pcm(pcm);
        std::array<uint8_t, 8> audio{};
        wait_for([&] { server.read_pcm(audio); return audio[0] == 123; });
        check(audio[2] == 0x38 && audio[3] == 0xfe, "PCM endian/stereo");
        std::array<uint8_t, 128> input{}; input[2] = 42; server.submit_input(input);
        wait_for([&] { return client.read_input(input) && input[2] == 42; });
        client.set_active(false);
        wait_for([&] { return server.read_video(frame, active) && !active && frame[0] == 16; });
        client.stop();
        wait_for([&] { return !server.connected(); });
        check(!client.read_input(input), "stale input disconnected");
        check(client.start(path, error), "reconnect start");
        wait_for([&] { return server.connected() && client.connected(); });
        client.stop();
        wait_for([&] { return !server.connected(); });
        // Malformed clients are disconnected, not allowed to poison the next frame.
        int raw = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        sockaddr_un address{}; address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        check(connect(raw, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "raw connect");
        wait_for([&] { return server.connected(); });
        const std::array<uint8_t, 16> invalid{};
        check(send(raw, invalid.data(), invalid.size(), MSG_NOSIGNAL) == 16, "invalid send");
        wait_for([&] { return !server.connected(); });
        close(raw);
        check(server.read_video(frame, active) && !active && frame[0] == 16, "idle survives bad client");
        // While connected, connector idle art takes precedence over service fallback logo.
        const auto logo = drc_ipc::AppHook::rgb_to_i420(std::vector<uint8_t>(108,100),6,6);
        check(server.set_idle_frame(logo), "set service logo");
        check(server.read_video(frame, active) && !active && frame == logo, "logo while disconnected");
        check(client.start(path, error), "logo test reconnect");
        wait_for([&] { return server.connected() && client.connected(); });
        client.submit_rgb(black,6,6,true);
        wait_for([&] { return server.read_video(frame, active) && !active && frame == converted; });
        client.set_active(true);
        wait_for([&] { server.read_video(frame,active); return active; });
        check(frame == converted, "connector logo until first active frame");
        client.submit_rgb(white,6,6);
        wait_for([&] { return server.read_video(frame,active) && active && frame[0] == 235; });
        // Static/paused active streams must not get replaced merely for being still.
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        check(server.read_video(frame,active) && active && frame[0] == 235, "hold active paused frame");
        client.set_active(false);
        wait_for([&] { return server.read_video(frame,active) && !active && frame == converted; });
        client.set_active(true);
        client.submit_rgb(white,6,6);
        wait_for([&] { return server.read_video(frame,active) && active && frame[0] == 235; });
        client.stop();
        wait_for([&] { return !server.connected(); });
        check(server.read_video(frame,active) && !active && frame == logo, "service logo replaces last game frame on disconnect");
        server.stop();
        check(access(path.c_str(), F_OK) != 0, "socket cleanup");
        rmdir(directory);
        std::cout << "AppHook tests passed\n";
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; rmdir(directory); return 1; }
}
