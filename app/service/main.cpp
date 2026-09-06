#include "service.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QTimer>
#include <QLockFile>
#include <csignal>
#include <unistd.h>
namespace { volatile sig_atomic_t stopping = 0; void Stop(int) { stopping = 1; } }
int main(int argc, char** argv)
{
    QCoreApplication app(argc,argv);
    if (geteuid() != 0) { qCritical("Run the installed system service, not a privileged GUI"); return 1; }
    QLockFile lock("/run/barista/service.lock"); lock.setStaleLockTime(0);
    if (!lock.tryLock()) { qCritical("Barista service already running or runtime directory missing"); return 1; }
    Service service;
    auto bus = QDBusConnection::systemBus();
    if (!bus.registerObject("/org/barista/Service1", &service, QDBusConnection::ExportAllSlots) ||
        !bus.registerService("org.barista.Service1")) { qCritical("Cannot register Barista system service"); return 1; }
    std::signal(SIGTERM,Stop); std::signal(SIGINT,Stop);
    QTimer signalPoll;
    QObject::connect(&signalPoll,&QTimer::timeout,&app,[&] { if (stopping) app.quit(); });
    signalPoll.start(100);
    return app.exec();
}
