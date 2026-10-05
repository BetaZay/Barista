#pragma once
#include <QString>

inline bool DesktopUsesPortal(const QString& platform, const QString& sessionType)
{
    return platform.startsWith("wayland") ||
           (platform == "xcb" && sessionType.compare("wayland", Qt::CaseInsensitive) == 0);
}
