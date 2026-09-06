#include "single_instance.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>

int main(int argc, char** argv)
{
    QCoreApplication app(argc,argv);
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    const auto path = directory.filePath("ui.sock");
    auto wait = [](auto condition) {
        QElapsedTimer clock; clock.start();
        while (!condition() && clock.elapsed() < 1500) { QCoreApplication::processEvents(); QThread::msleep(1); }
        return condition();
    };
    {
        SingleInstance primary(path), secondary(path);
        const auto primaryResult = primary.Start();
        if (primaryResult != SingleInstance::Result::Primary) { std::cerr << "primary " << int(primaryResult) << '\n'; return 1; }
        int shows = 0;
        QObject::connect(&primary,&SingleInstance::ShowRequested,[&] { ++shows; });
        if (secondary.Start() != SingleInstance::Result::Forwarded || !wait([&] { return shows == 1; })) { std::cerr << "secondary " << shows << '\n'; return 1; }
        QLocalSocket bad;
        bad.connectToServer(path);
        if (!bad.waitForConnected(1000)) { std::cerr << "bad-connect\n"; return 1; }
        bad.write("not-a-command\n"); bad.waitForBytesWritten(1000);
        if (!wait([&] { return bad.state() == QLocalSocket::UnconnectedState; }) || shows != 1) { std::cerr << "bad-close " << shows << '\n'; return 1; }
        SingleInstance third(path);
        if (third.Start() != SingleInstance::Result::Forwarded || !wait([&] { return shows == 2; })) { std::cerr << "third " << shows << '\n'; return 1; }
    }
    SingleInstance reopened(path);
    if (reopened.Start() != SingleInstance::Result::Primary) { std::cerr << "reopened\n"; return 1; }
    std::cout << "single-instance activation passed\n";
    return 0;
}
