#include "api/app_hook.h"
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
template<class Function> void Wait(Function condition)
{
    for (int count = 0; count < 400; ++count)
    {
        if (condition()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("keyboard IPC timeout");
}
}
int main()
{
    using namespace barista::api;
    char directory[] = "/tmp/barista-keyboard-ipc-XXXXXX";
    Check(mkdtemp(directory), "temporary directory");
    const auto path = std::string(directory) + "/media.sock";
    AppHook server(true), client(false);
    std::string error;
    Check(server.start(path, error), "start server");
    Check(client.start(path, error), "start client");
    Wait([&] { return client.connected() && server.connected(); });
    KeyboardRequest request{42, "Player name", "caf\xc3\xa9", 5, true};
    Check(client.request_keyboard(request), "request keyboard");
    KeyboardCommand command;
    Wait([&] { return server.read_keyboard_command(command); });
    Check(command.request.id == 42 && command.request.title == "Player name" &&
          command.request.initialText == request.initialText && command.request.maxCharacters == 5 &&
          command.request.password && !command.cancel, "request serialization");
    const auto revision = command.connectionRevision;
    Check(!client.submit_keyboard_result({42, KeyboardOutcome::Submitted, "no"}, revision), "client impersonated server");
    Check(!server.request_keyboard(request), "server impersonated client");
    Check(server.submit_keyboard_result({42, KeyboardOutcome::Submitted, "caf\xc3\xa9!"}, revision), "submit result");
    KeyboardResult result;
    Wait([&] { return client.read_keyboard_result(result); });
    Check(result.id == 42 && result.outcome == KeyboardOutcome::Submitted && result.text == "caf\xc3\xa9!", "result serialization");
    Check(!client.read_keyboard_result(result), "result repeated");
    Check(client.cancel_keyboard(42), "cancel request");
    Wait([&] { return server.read_keyboard_command(command); });
    Check(command.cancel && command.request.id == 42, "cancel serialization");
    Check(server.submit_keyboard_result({42, KeyboardOutcome::Cancelled, {}}, revision), "cancel result");
    Wait([&] { return client.read_keyboard_result(result); });
    Check(result.outcome == KeyboardOutcome::Cancelled && result.text.empty(), "cancel content");
    Check(server.submit_keyboard_result({99, KeyboardOutcome::Busy, {}}, revision), "busy result");
    Wait([&] { return client.read_keyboard_result(result); });
    Check(result.id == 99 && result.outcome == KeyboardOutcome::Busy, "busy outcome");
    request.id = 0;
    Check(!client.request_keyboard(request), "zero request ID");
    request.id = 42; request.maxCharacters = 3;
    Check(!client.request_keyboard(request), "character limit");
    request.maxCharacters = 1025;
    Check(!client.request_keyboard(request), "unbounded limit");
    request.maxCharacters = 10; request.initialText = std::string("\xed\xa0\x80");
    Check(!client.request_keyboard(request), "UTF-8 surrogate accepted");
    Check(!server.submit_keyboard_result({42, KeyboardOutcome::Cancelled, "secret"}, revision), "cancel leaked text");
    client.stop();
    Wait([&] { return !server.connected(); });
    Check(!server.submit_keyboard_result({42, KeyboardOutcome::Submitted, "old"}, revision), "disconnected result accepted");
    Check(client.start(path, error), "reconnect");
    Wait([&] { return client.connected() && server.connected(); });
    Check(server.connection_revision() != revision, "connection identity reused");
    Check(!server.submit_keyboard_result({42, KeyboardOutcome::Submitted, "old"}, revision), "old text reached replacement client");
    // Normal media/input remains usable by clients that never request a keyboard.
    std::array<uint8_t, 128> input{};
    input[3] = 0x80;
    server.submit_input(input);
    Wait([&] { std::array<uint8_t, 128> received; return client.read_input(received) && received == input; });
    client.stop(); server.stop();
    std::filesystem::remove_all(directory);
    std::cout << "keyboard IPC: UTF-8, requests/results, bounds, cancellation and connection ownership passed\n";
}
