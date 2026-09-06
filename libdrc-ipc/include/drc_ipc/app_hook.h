#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace drc_ipc
{
constexpr size_t Width = 864, Height = 480;
constexpr size_t FrameBytes = Width * Height * 3 / 2;

// MUG (Media User Gateway) is Barista's Linux-local AppHook transport. It lets
// any desktop application provide screen/audio and receive GamePad input; no
// pairing keys or hardware privileges cross this interface.
class AppHook
{
public:
    explicit AppHook(bool server);
    ~AppHook();
    bool start(const std::string& path, std::string& error);
    void stop();
    bool connected() const;
    void set_active(bool active);
    // Server-owned fallback, displayed when disconnected or when no connector idle art is present.
    // When connected, connector idle art takes precedence over the server fallback.
    bool set_idle_frame(std::span<const uint8_t> i420);
    void submit_rgb(std::vector<uint8_t> rgb, unsigned width, unsigned height, bool idle = false);
    void submit_pcm(std::span<const int16_t> stereo);
    void submit_input(std::span<const uint8_t> report);
    bool read_input(std::array<uint8_t, 128>& report) const;
    bool read_video(std::span<uint8_t> i420, bool& active);
    void read_pcm(std::span<uint8_t> s16le);
    struct ConnectedAppInfo
    {
        bool connected = false;
        uint32_t pid = 0;
        uint32_t uid = 0;
        std::string name;
        std::string idle_logo;
        uint64_t connected_at = 0;
        uint64_t last_seen = 0;
    };
    ConnectedAppInfo connected_app() const;
    std::string rejection_reason() const;
    uint64_t idle_revision() const;
    static bool read_app_lock(const std::string& socket_path, ConnectedAppInfo& info);
    static std::vector<uint8_t> rgb_to_i420(std::span<const uint8_t> rgb, unsigned width, unsigned height);
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
}
