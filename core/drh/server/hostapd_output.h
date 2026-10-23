#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace barista::drh
{
// Owned by the hostapd process, not by an individual reader. Startup hands
// unread output to the monitor, including lines split across pipe reads.
class HostapdOutput
{
public:
    void Append(std::string_view bytes) { m_buffer.append(bytes); }
    void Clear() { m_buffer.clear(); }

    std::optional<std::string> NextLine()
    {
        const auto end = m_buffer.find('\n');
        if (end == std::string::npos)
            return std::nullopt;
        auto line = m_buffer.substr(0, end);
        m_buffer.erase(0, end + 1);
        return line;
    }

private:
    std::string m_buffer;
};
}
