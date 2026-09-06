#include "window.h"
#include "single_instance.h"
#include <QApplication>
#include <QTimer>
#include <QIcon>
#include <QFileInfo>
#include <QTabWidget>
#include <QStandardPaths>
#include <QDir>
#include <QMessageBox>
#include <memory>
#ifdef BARISTA_LINUX_CONTROL
#include <unistd.h>
#endif
int main(int argc,char** argv)
{
    QApplication app(argc,argv);
#ifdef BARISTA_LINUX_CONTROL
    if (geteuid() == 0) { qCritical("Do not run the Barista GUI as root; use its polkit-backed service"); return 1; }
#endif
    app.setApplicationName("Barista"); app.setOrganizationName("Barista"); app.setApplicationVersion("0.1.0");
    app.setDesktopFileName("org.barista.Barista");
    app.setWindowIcon(QIcon(":/barista/barista-logo.png"));
    const bool smoke = app.arguments().contains("--smoke-test");
    std::unique_ptr<SingleInstance> instance;
    if (!smoke) {
        const auto runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (runtime.isEmpty() || !QDir().mkpath(runtime)) { qCritical("Cannot create the user runtime directory"); return 1; }
        instance = std::make_unique<SingleInstance>(runtime + "/barista-ui");
        const auto result = instance->Start();
        if (result == SingleInstance::Result::Forwarded) return 0;
        if (result == SingleInstance::Result::Error) {
            QMessageBox::warning(nullptr,"Barista is already starting","Could not reach the Barista window. Try its tray icon, or wait a moment and open it again."); return 1;
        }
        app.setQuitOnLastWindowClosed(false);
    }
    if (smoke && app.windowIcon().pixmap(32,32).isNull()) { qCritical("Embedded Barista logo is missing"); return 1; }
    Window window(smoke); window.show();
    if (instance) QObject::connect(instance.get(),&SingleInstance::ShowRequested,&window,&Window::ShowWindow);
    if (!smoke && app.arguments().contains("--background")) window.RunInBackground();
    if (smoke && app.arguments().contains("--smoke-pairing"))
        window.findChild<QTabWidget*>()->setCurrentIndex(1);
    if (smoke && app.arguments().contains("--smoke-advanced"))
        window.findChild<QTabWidget*>()->setCurrentIndex(2);
    if (smoke) QTimer::singleShot(200,&app,[&] {
        const auto index = app.arguments().indexOf("--smoke-screenshot");
        if (index >= 0) {
            const auto path = app.arguments().value(index+1);
            if (path.isEmpty() || QFileInfo::exists(path) || !window.grab().save(path)) { app.exit(1); return; }
        }
        window.Quit();
    });
    return app.exec();
}
