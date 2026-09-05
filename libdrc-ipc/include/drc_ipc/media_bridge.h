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

// Linux local-only raw-media bridge. No pairing keys or hardware privileges
// cross this interface. The worker owns the socket; callers never do I/O.
class MediaBridge
{
public:
    explicit MediaBridge(bool server);
    ~MediaBridge();
    bool start(const std::string& path, std::string& error);
    void stop();
    bool connected() const;
    void set_active(bool active);
    void submit_rgb(std::vector<uint8_t> rgb, unsigned width, unsigned height, bool idle = false);
    void submit_pcm(std::span<const int16_t> stereo);
    void submit_input(std::span<const uint8_t> report);
    bool read_input(std::array<uint8_t, 128>& report) const;
    bool read_video(std::span<uint8_t> i420, bool& active);
    void read_pcm(std::span<uint8_t> s16le);
    static std::vector<uint8_t> rgb_to_i420(std::span<const uint8_t> rgb, unsigned width, unsigned height);
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
}
