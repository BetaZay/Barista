#pragma once
#include <atomic>

namespace barista::drh
{
class VideoSendRecovery
{
public:
    // Producer thread consumes requests before encoding.
    bool ConsumeRequest() { return m_requested.exchange(false); }

    // Remaining methods run only on the serial sender thread.
    bool CanSend(bool idr)
    {
        if (!m_needsIdr || idr) return true;
        m_requested.store(true);
        return false;
    }
    void Failed()
    {
        m_needsIdr = true;
        m_requested.store(true);
    }
    void Delivered(bool idr)
    {
        if (idr) m_needsIdr = false;
    }

private:
    std::atomic_bool m_requested{false};
    bool m_needsIdr = false;
};
}
