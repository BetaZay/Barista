#pragma once
#include <QImage>
#include <QString>

namespace barista {
// Image-only rendering works in the headless service: no window or font system.
QImage IdleScreen();
bool WriteIdleScreen(const QString& path, QString& error);
}
