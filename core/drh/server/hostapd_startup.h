#pragma once

#include <string>
#include <string_view>

namespace barista::drh
{
// A NO-IR warning can precede hostapd's scan retry. Only a terminal event,
// process exit, or the caller's deadline makes that warning a startup failure.
class HostapdStartup
{
public:
    enum class Result { Waiting, Ready, Failed };

    Result Observe(std::string_view line)
    {
        if (line.find("not allowed for AP mode") != std::string_view::npos &&
            line.find("NO-IR") != std::string_view::npos)
            m_regulatoryBlocked = true;
        if (line.find("AP-ENABLED") != std::string_view::npos ||
            line.find("AP_ENABLED") != std::string_view::npos)
            return Result::Ready;
        if (line.find("AP-DISABLED") != std::string_view::npos ||
            line.find("AP-NO-IR") != std::string_view::npos)
        {
            m_regulatoryBlocked |= line.find("AP-NO-IR") != std::string_view::npos;
            return Result::Failed;
        }
        return Result::Waiting;
    }

    std::string FailureMessage(std::string_view fallback) const
    {
        return m_regulatoryBlocked
            ? "wireless regulatory domain marks the selected AP channel no-IR | " + std::string(fallback)
            : std::string(fallback);
    }

private:
    bool m_regulatoryBlocked = false;
};
}
