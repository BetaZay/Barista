#include "idle_screen.h"
#include "drc_ipc/app_hook.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <iostream>

int main(int argc, char** argv)
{
    QCoreApplication app(argc,argv);
    const auto screen = barista::IdleScreen();
    if (screen.size() != QSize(864,480) || screen.pixelColor(0,0) != QColor(24,24,24)) return 1;
    int logoPixels = 0;
    for (int y=0; y<screen.height(); ++y)
        for (int x=0; x<screen.width(); ++x)
            if (screen.pixelColor(x,y) != QColor(24,24,24)) ++logoPixels;
    if (logoPixels < 1000 || logoPixels > 320*320) return 1;
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    QString error;
    const auto path = directory.filePath("idle.i420");
    if (!barista::WriteIdleScreen(path,error)) { std::cerr << error.toStdString(); return 1; }
    QFile raw(path);
    if (!raw.open(QIODevice::ReadOnly) || raw.size() != qint64(drc_ipc::FrameBytes)) return 1;
    const auto expected = drc_ipc::AppHook::rgb_to_i420(
        {screen.constBits(),size_t(screen.sizeInBytes())},screen.width(),screen.height());
    if (raw.readAll() != QByteArray(reinterpret_cast<const char*>(expected.data()),expected.size())) return 1;
    if (barista::WriteIdleScreen(directory.filePath("missing/idle.i420"),error)) return 1;
    // Optional test artifact; never replace an existing image.
    if (argc == 2 && (QFileInfo::exists(argv[1]) || !screen.save(QString::fromLocal8Bit(argv[1])))) return 1;
    return 0;
}
