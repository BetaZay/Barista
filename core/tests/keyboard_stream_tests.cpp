#include "api/app_hook.h"
#include "api/controller.h"
#include "drh/encoder/media_streamer.h"
#include "drh/runtime_transport.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
template<class Function> void Wait(Function condition, const char* stage)
{
    for (int count = 0; count < 400; ++count)
    {
        if (condition()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error(std::string("keyboard engine timeout: ") + stage);
}
}
int main()
{
    using namespace barista::api;
    char directory[] = "/tmp/barista-keyboard-stream-XXXXXX";
    Check(mkdtemp(directory), "temporary directory");
    const auto path = std::string(directory) + "/media.sock";
    setenv("BARISTA_MUG_SOCKET", path.c_str(), 1);
    setenv("BARISTA_HOME_MENU", "1", 1);
    unsetenv("BARISTA_IDLE_I420");
    barista::drh::RuntimeTransport transport;
    std::string error;
    Check(transport.start({.console_address = "127.0.0.3", .gamepad_address = "127.0.0.4"}, error), "start mock transport");
    barista::drh::MediaStreamer media(transport, "");
    Check(media.start(error), "start engine");
    AppHook client(false);
    Check(client.start(path, error), "connect client");
    Wait([&] { return client.connected(); }, "connect");
    Check(client.request_keyboard({1, "Player name", "Link", 16}), "first request");
    Check(client.request_keyboard({2}), "second request");
    KeyboardResult result;
    Wait([&] { return client.read_keyboard_result(result); }, "result");
    Check(result.id == 2 && result.outcome == KeyboardOutcome::Busy, "engine did not open first keyboard");
    int input = socket(AF_INET, SOCK_DGRAM, 0);
    Check(input >= 0, "input socket");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(50022);
    inet_pton(AF_INET, "127.0.0.3", &address.sin_addr);
    sockaddr_in sender{}; sender.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.4", &sender.sin_addr);
    Check(bind(input, reinterpret_cast<sockaddr*>(&sender), sizeof(sender)) == 0, "bind mock GamePad source");
    uint8_t sequence = 1;
    auto send = [&](uint32_t buttons)
    {
        std::array<uint8_t, 128> report{};
        report[0] = sequence++;
        for (size_t offset = 6; offset < 14; offset += 2)
        {
            report[offset] = 2;
            report[offset + 1] = 8; // Native neutral center is 2050, not zero.
        }
        if (buttons) { report[6] = 0x80; report[7] = 0x0c; } // Moving stick must be consumed.
        report[2] = buttons >> 8; report[3] = buttons; report[80] = buttons >> 16;
        Check(sendto(input, report.data(), report.size(), 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 128, "send native input");
        Wait([&]
        {
            std::array<uint8_t, 128> received{};
            if (!client.read_input(received) || received[0] != report[0]) return false;
            Check(barista::DecodeInput(received).buttons == 0, "keyboard input escaped engine overlay");
            Check(barista::DecodeInput(received).sticks == std::array<int, 4>{}, "keyboard stick input escaped");
            return true;
        }, "native input");
    };
    send(0x8000); // Type q.
    send(0);
    send(0x8); // Done.
    Wait([&] { return client.read_keyboard_result(result); }, "result");
    Check(result.id == 1 && result.outcome == KeyboardOutcome::Submitted && result.text == "Linkq", "engine text round trip");
    send(0);
    Check(client.request_keyboard({3, "Old prompt", "private", 100, true}), "old prompt");
    Check(client.request_keyboard({4}), "busy probe");
    Wait([&] { return client.read_keyboard_result(result); }, "result");
    Check(result.id == 4 && result.outcome == KeyboardOutcome::Busy, "old prompt not open");
    client.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Check(client.start(path, error), "replacement connection");
    Wait([&] { return client.connected(); }, "connect");
    Check(client.request_keyboard({5, "New prompt", "", 100}), "replacement prompt");
    Check(client.request_keyboard({6}), "replacement busy probe");
    Wait([&] { return client.read_keyboard_result(result); }, "result");
    Check(result.id == 6 && result.outcome == KeyboardOutcome::Busy, "disconnected prompt blocked replacement");
    Check(client.cancel_keyboard(5), "cancel replacement");
    Wait([&] { return client.read_keyboard_result(result); }, "result");
    Check(result.id == 5 && result.outcome == KeyboardOutcome::Cancelled && result.text.empty(), "cancel engine prompt");
    client.stop(); media.stop(); transport.stop(); close(input);
    std::filesystem::remove_all(directory);
    std::cout << "keyboard engine: request, native input consumption, result and reconnect passed\n";
}
