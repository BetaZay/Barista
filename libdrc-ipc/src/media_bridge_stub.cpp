#include "drc_ipc/media_bridge.h"
#include <algorithm>
namespace drc_ipc
{
class MediaBridge::Impl {};
MediaBridge::MediaBridge(bool) : m_impl(std::make_unique<Impl>()) {}
MediaBridge::~MediaBridge() = default;
bool MediaBridge::start(const std::string&, std::string& error) { error = "local media IPC requires Linux"; return false; }
void MediaBridge::stop() {}
bool MediaBridge::connected() const { return false; }
void MediaBridge::set_active(bool) {}
void MediaBridge::submit_rgb(std::vector<uint8_t>, unsigned, unsigned, bool) {}
void MediaBridge::submit_pcm(std::span<const int16_t>) {}
void MediaBridge::submit_input(std::span<const uint8_t>) {}
bool MediaBridge::read_input(std::array<uint8_t, 128>&) const { return false; }
bool MediaBridge::read_video(std::span<uint8_t>, bool& active) { active = false; return false; }
void MediaBridge::read_pcm(std::span<uint8_t> pcm) { std::fill(pcm.begin(), pcm.end(), 0); }
std::vector<uint8_t> MediaBridge::rgb_to_i420(std::span<const uint8_t>, unsigned, unsigned) { return {}; }
}
