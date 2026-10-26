#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QVariantMap>
#include <csignal>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

bool Wait(const std::function<bool()>& ready)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < 5000)
    {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return ready();
}
}

class MockService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.barista.Service1")
public:
    QString owner, interface, mode, country, code;
    bool running = false, failStatus = false, denyStart = false, wrongOwner = false;
    int starts = 0, stops = 0, polls = 0, prepares = 0;
public slots:
    QVariantList SavedGamePads()
    {
        return {QVariantMap{{"mac", "02:00:00:00:00:01"}, {"name", "Test GamePad"}}};
    }
    QVariantMap GetStatus()
    {
        ++polls;
        return {{"phase", failStatus ? "failed" : running ? code.isEmpty() ? "runtime" : "pairing" : "idle"},
            {"running", running}, {"busy", false}, {"connected", running && code.isEmpty()},
            {"interface", interface}, {"mode", mode}, {"ownedByCaller", owner == message().service()},
            {"error", failStatus ? "Mock engine failure" : ""}};
    }
    QVariantMap GetDiagnostics() { return {{"report", "Mock support report\n"}}; }
    void PrepareSystem() { ++prepares; }
    void StartSessionWithCountry(const QString& adapter, const QString& requestedMode, const QString& requestedCountry)
    {
        ++starts;
        if (denyStart)
        {
            sendErrorReply("org.barista.Error.Unauthorized", "Mock authorization denied");
            return;
        }
        owner = message().service();
        interface = adapter;
        mode = requestedMode;
        country = requestedCountry;
        code.clear();
        running = true;
    }
    void PairWithCountry(const QString& adapter, const QString& pattern,
        const QString& requestedMode, const QString& requestedCountry)
    {
        StartSessionWithCountry(adapter, requestedMode, requestedCountry);
        code = pattern;
        // Exercise --stop while authorization/setup is still pending.
        setDelayedReply(true);
        const auto response = message().createReply();
        const auto bus = connection();
        QTimer::singleShot(200, this, [bus, response] { bus.send(response); });
    }
    void StopSession()
    {
        ++stops;
        if (owner != message().service())
        {
            wrongOwner = true;
            sendErrorReply("org.barista.Error.Owner", "Session belongs to another client");
            return;
        }
        running = false;
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try
    {
        Check(argc == 3, "expected CLI and GUI executable paths");
        QTemporaryDir settings;
        Check(settings.isValid(), "temporary settings directory");
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("DBUS_SYSTEM_BUS_ADDRESS", environment.value("DBUS_SESSION_BUS_ADDRESS"));
        environment.insert("XDG_CONFIG_HOME", settings.path());
        environment.remove("DISPLAY");
        environment.remove("WAYLAND_DISPLAY");
        environment.insert("QT_QPA_PLATFORM", "barista-invalid-display");
        auto bus = QDBusConnection::sessionBus();
        MockService service;
        Check(bus.registerObject("/org/barista/Service1", &service, QDBusConnection::ExportAllSlots), "mock object");
        Check(bus.registerService("org.barista.Service1"), "mock service");
        const QString cli = app.arguments()[1], gui = app.arguments()[2];
        auto launch = [&](QProcess& process, const QString& executable, const QStringList& arguments) {
            process.setProcessEnvironment(environment);
            process.start(executable, arguments);
            Check(process.waitForStarted(), "start CLI process");
        };
        auto finish = [&](QProcess& process, int expected) {
            Check(Wait([&] { return process.state() == QProcess::NotRunning; }), "CLI timed out");
            const auto errors = process.readAllStandardError();
            if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != expected)
                throw std::runtime_error(("unexpected CLI exit: " + errors).constData());
            return process.readAllStandardOutput() + errors;
        };
        auto run = [&](const QString& executable, const QStringList& arguments, int expected = 0) {
            QProcess process;
            launch(process, executable, arguments);
            return finish(process, expected);
        };
        for (const auto& executable : {cli, gui})
        {
            Check(run(executable, {"--help"}).contains("--pair"), "headless help");
            Check(run(executable, {"--help-all"}).contains("--start"), "extended help");
            Check(run(executable, {"--version"}).contains("Barista"), "headless version");
            Check(run(executable, {"--list"}).contains("Test GamePad"), "saved GamePad list");
            Check(run(executable, {"--list"}).contains("02:00:00:00:00:01"), "saved GamePad MAC");
            Check(run(executable, {"--status"}).contains("Phase: idle"), "headless status");
            Check(run(executable, {"--diagnostics"}).contains("Mock support report"), "diagnostics");
            run(executable, {"--interfaces"});
            for (const auto& invalid : QList<QStringList>{
                {"--unknown"}, {"--list", "--start"}, {"--start", "--interface", "../bad"},
                {"--pair", "--code", "0124"}, {"--start", "--code", "0123"},
                {"--start", "--mode", "desktop"}, {"--start", "--country", "USA"},
                {"--status", "--interface", "wlan0"}, {"--pair", "unexpected"}})
                run(executable, invalid, 2);
        }
        Check(service.starts == 0, "invalid arguments must not start a session");
        run(cli, {"--prepare"});
        Check(service.prepares == 1, "prepare operation");
        run(cli, {"--stop"}, 1);

        QProcess owner;
        launch(owner, gui, {"--start", "--interface", "mock0", "--mode", "controller", "--country", "us"});
        Check(Wait([&] { return service.running && service.polls > 2; }), "foreground start");
        Check(owner.state() == QProcess::Running, "owner must remain alive");
        Check(service.interface == "mock0" && service.mode == "controller" && service.country == "US", "start arguments");
        run(cli, {"--start", "--interface", "mock0"}, 1);
        Check(service.starts == 1, "second CLI must not start another session");
        run(cli, {"--stop"});
        finish(owner, 0);
        Check(service.stops == 1 && !service.running && !service.wrongOwner, "stop through original owner");

        launch(owner, cli, {"--pair", "--interface", "mock0", "--code", "0123"});
        Check(Wait([&] { return service.running; }), "pair start");
        Check(service.code == "0123" && service.mode == "real", "pair arguments");
        Check(Wait([&] { return owner.bytesAvailable() > 0; }), "pair output");
        QByteArray transcript;
        Check(Wait([&] { transcript += owner.readAllStandardOutput(); return transcript.contains("Pair now:"); }), "pair ready");
        Check(transcript.contains("Spade  Heart  Diamond  Club"), "pairing symbol order");
        owner.terminate();
        finish(owner, 0);
        Check(service.stops == 2 && !service.wrongOwner, "SIGTERM cleanup");

        launch(owner, cli, {"--pair", "--interface", "mock0"});
        Check(Wait([&] { return service.running; }), "random pair start");
        Check(service.code.size() == 4, "random pairing code");
        run(gui, {"--stop"});
        finish(owner, 0);
        Check(service.stops == 3 && !service.running, "stop during pending pair");

        launch(owner, cli, {"--start", "--interface", "mock0"});
        Check(Wait([&] { return service.running; }), "start before interrupt");
        Check(::kill(static_cast<pid_t>(owner.processId()), SIGINT) == 0, "send Ctrl+C");
        finish(owner, 0);
        Check(service.stops == 4 && !service.running, "SIGINT cleanup");

        service.failStatus = true;
        Check(run(cli, {"--start", "--interface", "mock0"}, 1).contains("Mock engine failure"), "engine failure surfaced");
        Check(service.stops == 5 && !service.running, "cleanup after engine failure");
        service.failStatus = false;
        service.denyStart = true;
        Check(run(cli, {"--start", "--interface", "mock0"}, 1).contains("Mock authorization denied"), "authorization failure surfaced");
        Check(service.stops == 5, "rejected start does not stop a session");

        bus.unregisterService("org.barista.Service1");
        Check(!run(gui, {"--status"}, 1).isEmpty(), "service unavailable surfaced");
        std::cout << "CLI arguments, headless dispatch, D-Bus operations and ownership passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#include "cli_test.moc"
