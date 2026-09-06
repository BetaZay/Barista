#include "idle_screen.h"
#include "drc_ipc/app_hook.h"
#include <QPainter>
#include <QSaveFile>

namespace barista {
QImage IdleScreen()
{
    const QImage logo(":/barista/barista-logo.png");
    if (logo.isNull()) return {};
    QImage screen(drc_ipc::Width, drc_ipc::Height, QImage::Format_RGB888);
    screen.fill(QColor(24,24,24));
    const auto scaled = logo.scaled(320,320,Qt::KeepAspectRatio,Qt::SmoothTransformation);
    QPainter painter(&screen);
    painter.drawImage((screen.width()-scaled.width())/2,(screen.height()-scaled.height())/2,scaled);
    return screen;
}
bool WriteIdleScreen(const QString& path, QString& error)
{
    const auto screen = IdleScreen();
    if (screen.isNull()) { error = "The embedded Barista logo could not be loaded"; return false; }
    // 864 RGB pixels have no scanline padding.
    const auto i420 = drc_ipc::AppHook::rgb_to_i420(
        {screen.constBits(), size_t(screen.sizeInBytes())}, screen.width(), screen.height());
    if (i420.size() != drc_ipc::FrameBytes) { error = "Could not prepare the Barista idle screen"; return false; }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(reinterpret_cast<const char*>(i420.data()), i420.size()) != qint64(i420.size()) || !file.commit())
    { error = "Could not save the Barista idle screen: " + file.errorString(); return false; }
    return true;
}
}
