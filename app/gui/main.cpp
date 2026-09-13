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
#include <QDialog>
#include <memory>
#ifdef BARISTA_LINUX_CONTROL
#include <unistd.h>
#endif
#ifndef BARISTA_VERSION_STRING
#define BARISTA_VERSION_STRING "0.1.0"
#endif

int main(int argc,char** argv)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--version" || arg == "-v") {
            std::printf("Barista %s\n", BARISTA_VERSION_STRING);
            return 0;
        }
    }
    QApplication app(argc,argv);
#ifdef BARISTA_LINUX_CONTROL
    if (geteuid() == 0) { qCritical("Do not run the Barista GUI as root; use its polkit-backed service"); return 1; }
#endif
    app.setApplicationName("Barista"); app.setOrganizationName("Barista"); app.setApplicationVersion(BARISTA_VERSION_STRING);
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
        window.OpenPairing();
    if (smoke && (app.arguments().contains("--smoke-advanced") ||
        app.arguments().contains("--smoke-settings") || app.arguments().contains("--smoke-about") ||
        app.arguments().contains("--smoke-connection"))) {
        window.findChild<QTabWidget*>("mainPages")->setCurrentIndex(2);
        window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(
            app.arguments().contains("--smoke-about") ? 3 : app.arguments().contains("--smoke-advanced") ? 2 :
            app.arguments().contains("--smoke-connection") ? 1 : 0);
    }
    if (smoke) QTimer::singleShot(200,&app,[&] {
        const auto index = app.arguments().indexOf("--smoke-screenshot");
        if (index >= 0) {
            const auto path = app.arguments().value(index+1);
            QWidget* target = &window;
            if (app.arguments().contains("--smoke-pairing"))
                target = window.findChild<QDialog*>("pairingDialog");
            if (!target || path.isEmpty() || QFileInfo::exists(path) || !target->grab().save(path)) { app.exit(1); return; }
        }
        window.Quit();
    });
    return app.exec();
}
