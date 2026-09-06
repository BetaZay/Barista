#include "drc_ipc/app_hook.h"
#include <algorithm>
namespace drc_ipc
{
class AppHook::Impl {};
AppHook::AppHook(bool) : m_impl(std::make_unique<Impl>()) {}
AppHook::~AppHook() = default;
bool AppHook::start(const std::string&, std::string& error) { error = "local media IPC requires Linux"; return false; }
void AppHook::stop() {}
bool AppHook::connected() const { return false; }
void AppHook::set_active(bool) {}
bool AppHook::set_idle_frame(std::span<const uint8_t>) { return false; }
void AppHook::submit_rgb(std::vector<uint8_t>, unsigned, unsigned, bool) {}
void AppHook::submit_pcm(std::span<const int16_t>) {}
void AppHook::submit_input(std::span<const uint8_t>) {}
bool AppHook::read_input(std::array<uint8_t, 128>&) const { return false; }
bool AppHook::read_video(std::span<uint8_t>, bool& active) { active = false; return false; }
void AppHook::read_pcm(std::span<uint8_t> pcm) { std::fill(pcm.begin(), pcm.end(), 0); }
AppHook::ConnectedAppInfo AppHook::connected_app() const { return {}; }
std::string AppHook::rejection_reason() const { return {}; }
uint64_t AppHook::idle_revision() const { return 0; }
bool AppHook::read_app_lock(const std::string&, ConnectedAppInfo&) { return false; }
std::vector<uint8_t> AppHook::rgb_to_i420(std::span<const uint8_t>, unsigned, unsigned) { return {}; }
}
