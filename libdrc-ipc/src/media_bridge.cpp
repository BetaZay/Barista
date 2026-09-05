#include "drc_ipc/media_bridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace drc_ipc
{
namespace
{
using Clock = std::chrono::steady_clock;
constexpr size_t Chunk = 16384;
constexpr size_t MaxAudioSamples = 4800 * 2; // bounded to 100ms, never grow latency
enum Type : uint32_t { Video = 1, Idle = 2, Active = 3, Pcm = 4, Input = 5 };
// Fixed little-endian local protocol: magic, type, frame ID, byte offset.
void put(uint8_t* p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = v >> (8 * i); }
uint32_t get(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
struct Rgb { std::vector<uint8_t> bytes; unsigned width = 0, height = 0; };
}

class MediaBridge::Impl
{
public:
    explicit Impl(bool server) : server(server) {}
    bool server;
    std::atomic_bool stopping{false}, linked{false};
    std::thread worker;
    mutable std::mutex mutex;
    Rgb pending, idle_rgb;
    std::vector<uint8_t> video, idle;
    std::deque<int16_t> audio;
    std::array<uint8_t, 128> input{};
    Clock::time_point input_time{}, video_time{}, heartbeat{};
    bool active = false, input_pending = false;
    uint64_t idle_revision = 0;
    int listener = -1;
    std::string path;
    dev_t socket_dev{};
    ino_t socket_ino{};
    uid_t allowed_uid = geteuid();

    bool send_packet(int fd, Type type, uint32_t id, uint32_t offset, std::span<const uint8_t> payload)
    {
        std::array<uint8_t, 16 + Chunk> packet{};
        if (payload.size() > Chunk) return false;
        put(packet.data(), 0x314d5244); // DRM1
        put(packet.data() + 4, type); put(packet.data() + 8, id); put(packet.data() + 12, offset);
        std::copy(payload.begin(), payload.end(), packet.begin() + 16);
        const auto deadline = Clock::now() + std::chrono::milliseconds(100);
        do
        {
            const auto n = send(fd, packet.data(), payload.size() + 16, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n == static_cast<ssize_t>(payload.size() + 16)) return true;
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
            pollfd p{fd, POLLOUT, 0}; poll(&p, 1, 2);
        } while (!stopping && Clock::now() < deadline);
        return false;
    }

    bool send_frame(int fd, Type type, uint32_t id, const std::vector<uint8_t>& frame)
    {
        for (size_t offset = 0; offset < frame.size(); offset += Chunk)
            if (!send_packet(fd, type, id, offset, std::span(frame).subspan(offset, std::min(Chunk, frame.size() - offset))))
                return false;
        return true;
    }

    void session(int fd)
    {
        linked = true;
        std::vector<uint8_t> assembly;
        uint32_t assembly_id = 0, assembly_type = 0, frame_id = 0;
        uint64_t sent_idle = 0;
        auto next_heartbeat = Clock::time_point{};
        auto last_received = Clock::now();
        while (!stopping)
        {
            if (!server)
            {
                Rgb frame, logo;
                std::vector<uint8_t> pcm;
                bool game_active;
                {
                    std::lock_guard lock(mutex);
                    frame = std::move(pending); pending = {};
                    if (sent_idle != idle_revision) { logo = idle_rgb; sent_idle = idle_revision; }
                    game_active = active;
                    while (!audio.empty() && pcm.size() + 2 <= Chunk)
                    {
                        uint16_t v = static_cast<uint16_t>(audio.front()); audio.pop_front();
                        pcm.push_back(v); pcm.push_back(v >> 8);
                    }
                }
                if (Clock::now() >= next_heartbeat)
                {
                    const std::array<uint8_t, 1> flag{static_cast<uint8_t>(game_active)};
                    if (!send_packet(fd, Active, 0, 0, flag)) break;
                    next_heartbeat = Clock::now() + std::chrono::milliseconds(100);
                }
                if (!logo.bytes.empty() && !send_frame(fd, Idle, ++frame_id,
                    MediaBridge::rgb_to_i420(logo.bytes, logo.width, logo.height))) break;
                if (!frame.bytes.empty() && !send_frame(fd, Video, ++frame_id,
                    MediaBridge::rgb_to_i420(frame.bytes, frame.width, frame.height))) break;
                if (!pcm.empty() && !send_packet(fd, Pcm, 0, 0, pcm)) break;
            }
            else
            {
                std::array<uint8_t, 128> report;
                bool available;
                {
                    std::lock_guard lock(mutex);
                    report = input; available = input_pending; input_pending = false;
                }
                if (available && !send_packet(fd, Input, 0, 0, report)) break;
                if (Clock::now() - last_received > std::chrono::seconds(2)) break;
            }

            pollfd poll_fd{fd, POLLIN, 0};
            poll(&poll_fd, 1, 2);
            if (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
            // Bound each batch so outgoing input/audio cannot starve.
            bool valid = true;
            for (unsigned batch = 0; batch < 64; ++batch)
            {
                std::array<uint8_t, 16 + Chunk> packet;
                const auto n = recv(fd, packet.data(), packet.size(), MSG_DONTWAIT | MSG_TRUNC);
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
                if (n < 16 || n > static_cast<ssize_t>(packet.size()) || get(packet.data()) != 0x314d5244)
                { valid = false; break; }
                last_received = Clock::now();
                const auto type = get(packet.data() + 4), id = get(packet.data() + 8), offset = get(packet.data() + 12);
                const auto payload = std::span(packet).subspan(16, n - 16);
                if (server && (type == Video || type == Idle))
                {
                    if (offset == 0) { assembly.clear(); assembly_id = id; assembly_type = type; }
                    if (id != assembly_id || type != assembly_type || offset != assembly.size() ||
                        payload.empty() || offset + payload.size() > FrameBytes) { valid = false; break; }
                    assembly.insert(assembly.end(), payload.begin(), payload.end());
                    if (assembly.size() == FrameBytes)
                    {
                        std::lock_guard lock(mutex);
                        if (type == Idle) idle = std::move(assembly);
                        else { video = std::move(assembly); video_time = Clock::now(); }
                        assembly.clear();
                    }
                }
                else if (server && type == Active && payload.size() == 1 && payload[0] <= 1)
                {
                    std::lock_guard lock(mutex);
                    active = payload[0]; heartbeat = Clock::now();
                    if (!active) { audio.clear(); video.clear(); }
                }
                else if (server && type == Pcm && payload.size() % 4 == 0)
                {
                    std::lock_guard lock(mutex);
                    if (active)
                        for (size_t i = 0; i < payload.size(); i += 2)
                            audio.push_back(static_cast<int16_t>(uint16_t(payload[i]) | uint16_t(payload[i + 1]) << 8));
                    while (audio.size() > MaxAudioSamples) audio.pop_front();
                }
                else if (!server && type == Input && payload.size() == 128)
                {
                    std::lock_guard lock(mutex);
                    std::copy(payload.begin(), payload.end(), input.begin()); input_time = Clock::now();
                }
                else { valid = false; break; }
            }
            if (!valid) break;
        }
        linked = false;
        close(fd);
        std::lock_guard lock(mutex);
        audio.clear(); video.clear(); input_time = {}; input_pending = false;
        if (server) { active = false; heartbeat = {}; }
    }

    void run()
    {
        while (!stopping)
        {
            int fd = -1;
            if (server)
            {
                pollfd p{listener, POLLIN, 0}; poll(&p, 1, 100);
                if (p.revents & POLLIN) fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
                if (fd >= 0)
                {
                    ucred cred{}; socklen_t size = sizeof(cred);
                    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) != 0 ||
                        (cred.uid != allowed_uid && cred.uid != 0)) { close(fd); fd = -1; }
                }
            }
            else
            {
                fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
                sockaddr_un address{}; address.sun_family = AF_UNIX;
                std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
                if (fd >= 0 && connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
                { close(fd); fd = -1; }
                if (fd < 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (fd >= 0) session(fd);
        }
    }
};

MediaBridge::MediaBridge(bool server) : m_impl(std::make_unique<Impl>(server)) {}
MediaBridge::~MediaBridge() { stop(); }
bool MediaBridge::start(const std::string& path, std::string& error)
{
    if (m_impl->worker.joinable()) { error = "media bridge already started"; return false; }
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) { error = "invalid media socket path"; return false; }
    auto& s = *m_impl; s.path = path; s.stopping = false;
    if (s.server)
    {
        // Never unlink a pre-existing endpoint: another daemon may own it.
        s.listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        sockaddr_un address{}; address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        if (s.listener < 0 || bind(s.listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        {
            error = "media socket bind: " + std::string(std::strerror(errno));
            if (s.listener >= 0) close(s.listener);
            s.listener = -1; return false;
        }
        struct stat info{}; lstat(path.c_str(), &info); s.socket_dev = info.st_dev; s.socket_ino = info.st_ino;
        if (geteuid() == 0)
            if (const char* uid = std::getenv("SUDO_UID"))
            {
                char* end = nullptr; const auto value = std::strtoul(uid, &end, 10);
                if (*uid && end && !*end && value <= UINT32_MAX) s.allowed_uid = static_cast<uid_t>(value);
            }
        if (chown(path.c_str(), s.allowed_uid, static_cast<gid_t>(-1)) != 0 || chmod(path.c_str(), 0600) != 0 || listen(s.listener, 1) != 0)
        { error = "media socket permissions/listen: " + std::string(std::strerror(errno)); stop(); return false; }
    }
    s.worker = std::thread([&s] { s.run(); });
    return true;
}
void MediaBridge::stop()
{
    auto& s = *m_impl; s.stopping = true;
    if (s.worker.joinable()) s.worker.join();
    if (s.listener >= 0)
    {
        close(s.listener); s.listener = -1;
        struct stat info{};
        if (lstat(s.path.c_str(), &info) == 0 && info.st_dev == s.socket_dev && info.st_ino == s.socket_ino)
            unlink(s.path.c_str());
    }
}
bool MediaBridge::connected() const { return m_impl->linked; }
void MediaBridge::set_active(bool active)
{
    std::lock_guard lock(m_impl->mutex); m_impl->active = active;
    if (!active) { m_impl->audio.clear(); m_impl->pending = {}; }
}
void MediaBridge::submit_rgb(std::vector<uint8_t> rgb, unsigned width, unsigned height, bool idle)
{
    if (!width || !height || width > 8192 || height > 8192 || rgb.size() != size_t(width) * height * 3) return;
    std::unique_lock lock(m_impl->mutex, std::try_to_lock);
    if (!lock) return; // GPU thread never waits for the daemon
    if (idle) { m_impl->idle_rgb = {std::move(rgb), width, height}; ++m_impl->idle_revision; }
    else m_impl->pending = {std::move(rgb), width, height};
}
void MediaBridge::submit_pcm(std::span<const int16_t> stereo)
{
    if (stereo.size() % 2 || stereo.size() > MaxAudioSamples) return;
    std::unique_lock lock(m_impl->mutex, std::try_to_lock);
    if (!lock || !m_impl->active || !m_impl->linked) return;
    m_impl->audio.insert(m_impl->audio.end(), stereo.begin(), stereo.end());
    while (m_impl->audio.size() > MaxAudioSamples) m_impl->audio.pop_front();
}
void MediaBridge::submit_input(std::span<const uint8_t> report)
{
    if (report.size() != 128) return;
    std::lock_guard lock(m_impl->mutex);
    std::copy(report.begin(), report.end(), m_impl->input.begin()); m_impl->input_pending = true;
}
bool MediaBridge::read_input(std::array<uint8_t, 128>& report) const
{
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->linked || Clock::now() - m_impl->input_time > std::chrono::milliseconds(500)) return false;
    report = m_impl->input; return true;
}
bool MediaBridge::read_video(std::span<uint8_t> i420, bool& active)
{
    if (i420.size() != FrameBytes) return false;
    std::lock_guard lock(m_impl->mutex);
    active = m_impl->linked && m_impl->active && Clock::now() - m_impl->heartbeat < std::chrono::seconds(1);
    const auto& frame = active && !m_impl->video.empty() ? m_impl->video : m_impl->idle;
    if (frame.size() != FrameBytes) return false;
    std::copy(frame.begin(), frame.end(), i420.begin()); return true;
}
void MediaBridge::read_pcm(std::span<uint8_t> output)
{
    std::fill(output.begin(), output.end(), 0);
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->linked || !m_impl->active || Clock::now() - m_impl->heartbeat > std::chrono::seconds(1))
    { m_impl->audio.clear(); return; }
    for (size_t i = 0; i + 3 < output.size() && m_impl->audio.size() >= 2; i += 4)
        for (size_t channel = 0; channel < 2; ++channel)
        {
            uint16_t v = static_cast<uint16_t>(m_impl->audio.front()); m_impl->audio.pop_front();
            output[i + channel * 2] = v; output[i + channel * 2 + 1] = v >> 8;
        }
}
std::vector<uint8_t> MediaBridge::rgb_to_i420(std::span<const uint8_t> rgb, unsigned width, unsigned height)
{
    if (!width || !height || width > 8192 || height > 8192 || rgb.size() != size_t(width) * height * 3) return {};
    std::vector<uint8_t> out(FrameBytes, 128);
    // RGB full range -> BT.601 limited-range I420, scale to the fixed DRC surface.
    for (size_t y = 0; y < Height; y += 2)
        for (size_t x = 0; x < Width; x += 2)
        {
            int rsum = 0, gsum = 0, bsum = 0;
            for (size_t dy = 0; dy < 2; ++dy)
                for (size_t dx = 0; dx < 2; ++dx)
                {
                    size_t offset = (((y + dy) * height / Height) * width + (x + dx) * width / Width) * 3;
                    int r = rgb[offset], g = rgb[offset + 1], b = rgb[offset + 2];
                    out[(y + dy) * Width + x + dx] = std::clamp(((66*r + 129*g + 25*b + 128) >> 8) + 16, 16, 235);
                    rsum += r; gsum += g; bsum += b;
                }
            const int r = rsum / 4, g = gsum / 4, b = bsum / 4;
            size_t chroma = (y / 2) * (Width / 2) + x / 2;
            out[Width * Height + chroma] = std::clamp(((-38*r - 74*g + 112*b + 128) >> 8) + 128, 16, 240);
            out[Width * Height * 5 / 4 + chroma] = std::clamp(((112*r - 94*g - 18*b + 128) >> 8) + 128, 16, 240);
        }
    return out;
}
}
