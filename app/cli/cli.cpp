#include "cli.h"
#include "api/types.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSettings>
#include <QTextStream>
#include <QTimer>
#include <csignal>
#include <string_view>
#ifdef BARISTA_LINUX_CONTROL
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#endif

namespace barista::cli
{
namespace
{
volatile std::sig_atomic_t interrupted = 0;
void Interrupt(int) { interrupted = 1; }

QStringList Interfaces()
{
    QStringList result;
    for (const auto& name : QDir("/sys/class/net").entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        if (QFileInfo::exists("/sys/class/net/" + name + "/phy80211")) result.append(name);
    return result;
}

#ifdef BARISTA_LINUX_CONTROL
constexpr auto CliService = "org.barista.Cli1";
constexpr auto CliPath = "/org/barista/Cli1";

// A second invocation asks the existing owner to stop. The privileged service's
// caller ownership rules remain unchanged, including cleanup on disconnection.
class SessionControl : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.barista.Cli1")
public:
    bool stopping = false;
public slots:
    void Stop() { stopping = true; }
};

QDBusMessage Call(const QDBusConnection& bus, const QString& method,
    const QVariantList& arguments = {}, bool cli = false)
{
    auto message = QDBusMessage::createMethodCall(cli ? CliService : "org.barista.Service1",
        cli ? CliPath : "/org/barista/Service1", cli ? CliService : "org.barista.Service1", method);
    message.setArguments(arguments);
    message.setAutoStartService(!cli);
    QDBusPendingCallWatcher watcher(bus.asyncCall(message, 150000));
    QEventLoop loop;
    QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
    if (!watcher.isFinished()) loop.exec();
    return watcher.reply();
}

void Wait()
{
    QEventLoop loop;
    QTimer::singleShot(250, &loop, &QEventLoop::quit);
    loop.exec();
}

void PrintStatus(const QVariantMap& status, QTextStream& out)
{
    out << "Phase: " << status.value("phase").toString()
        << "\nInterface: " << status.value("interface").toString()
        << "\nMode: " << status.value("mode").toString()
        << "\nGamePad connected: " << (status.value("connected").toBool() ? "yes" : "no") << '\n';
    if (status.value("batteryAvailable").toBool())
        out << "Battery: " << status.value("battery").toUInt() << "%\n";
    if (status.value("appConnected").toBool())
        out << "Application: " << status.value("appName").toString() << '\n';
    if (!status.value("error").toString().isEmpty())
        out << "Error: " << status.value("error").toString() << '\n'
            << status.value("errorAction").toString() << '\n';
    out.flush();
}
#endif
}

bool Requested(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg(argv[i]);
        if (arg == "--smoke-screenshot") { ++i; continue; }
        if (arg == "--background" || arg.starts_with("--smoke-")) continue;
        return true;
    }
    return false;
}

int Run(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("Barista");
    app.setOrganizationName("Barista");
    app.setApplicationVersion(BARISTA_VERSION_STRING);
    QCommandLineParser parser;
    parser.setApplicationDescription("Control Barista from the terminal. Run barista without options to open the GUI.\n"
        "Start and pair stay in the foreground; Ctrl+C or barista --stop ends the session.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"list", "List saved GamePads and wireless adapters."});
    parser.addOption({"interfaces", "List local wireless adapters without contacting the service."});
    parser.addOption({"status", "Show the current session."});
    parser.addOption({"pair", "Pair a GamePad, then keep its session running."});
    parser.addOption({"start", "Connect an already paired GamePad."});
    parser.addOption({"stop", "Stop this user's CLI session in the same desktop login."});
    parser.addOption({"prepare", "Prepare required host facilities through Polkit."});
    parser.addOption({"diagnostics", "Print the service support report."});
    parser.addOption({{"i", "interface"}, "Wireless adapter (saved GUI adapter or sole adapter by default).", "name"});
    parser.addOption({{"m", "mode"}, "Session mode: real (AppHook) or controller.", "mode", "real"});
    parser.addOption({"code", "Four pairing digits, each 0..3 (random by default).", "digits"});
    parser.addOption({"country", "Two-letter country code for your physical location.", "country"});
    QTextStream out(stdout), error(stderr);
    auto fail = [&error](const QString& message, int code = 1) {
        error << "barista: " << message << '\n';
        error.flush();
        return code;
    };
    if (!parser.parse(app.arguments())) return fail(parser.errorText(), 2);
    if (parser.isSet("help") || parser.isSet("help-all")) { out << parser.helpText(); return 0; }
    if (parser.isSet("version")) { out << "Barista " << app.applicationVersion() << '\n'; return 0; }
    QString action;
    for (const auto* name : {"list", "interfaces", "status", "pair", "start", "stop", "prepare", "diagnostics"})
    {
        if (!parser.isSet(name)) continue;
        if (!action.isEmpty()) return fail("Choose exactly one action. See --help.", 2);
        action = name;
    }
    if (action.isEmpty() || !parser.positionalArguments().isEmpty())
        return fail("Choose one action such as --list, --pair, or --start. See --help.", 2);
    const bool session = action == "start" || action == "pair";
    for (const auto* option : {"interface", "mode", "country", "code"})
        if (parser.isSet(option) && (!session || (QString(option) == "code" && action != "pair")))
            return fail(QString("--%1 is only valid with %2.").arg(option,
                QString(option) == "code" ? "--pair" : "--start or --pair"), 2);
    const auto mode = api::ParseSessionMode(parser.value("mode").toStdString());
    if (!mode || (*mode != api::SessionMode::Real && *mode != api::SessionMode::Controller))
        return fail("Choose --mode real or controller. Desktop and game mirroring use the GUI's capture controls.", 2);
    const auto country = parser.value("country").toUpper();
    if (!country.isEmpty() && !api::ValidRegulatoryCountry(country.toStdString()))
        return fail("--country must be a two-letter country code, such as US.", 2);
    QString code = parser.value("code");
    if (parser.isSet("code") && !api::ParsePairCode(code.toStdString()))
        return fail("--code must contain exactly four digits, each 0..3.", 2);
    const auto interfaces = Interfaces();
    if (action == "interfaces" || action == "list")
    {
        out << "Wireless adapters:\n";
        for (const auto& name : interfaces) out << "  " << name << '\n';
        if (interfaces.isEmpty()) out << "  None found.\n";
        out.flush();
        if (action == "interfaces") return 0;
    }
    QString interface = parser.value("interface");
    if (session)
    {
        if (interface.isEmpty())
        {
            const auto saved = QSettings().value("interface").toString();
            if (interfaces.contains(saved)) interface = saved;
            else if (interfaces.size() == 1) interface = interfaces.front();
        }
        if (!api::ValidInterfaceName(interface.toStdString()))
            return fail("Choose an adapter with --interface NAME. Use --interfaces to list adapters.", 2);
        if (action == "pair" && code.isEmpty())
            for (int i = 0; i < 4; ++i) code += QString::number(QRandomGenerator::global()->bounded(4u));
    }
#ifndef BARISTA_LINUX_CONTROL
    return fail("GamePad session control requires the Linux Barista service.");
#else
    auto systemBus = QDBusConnection::systemBus();
    if (action == "stop")
    {
        const auto reply = Call(QDBusConnection::sessionBus(), "Stop", {}, true);
        if (reply.type() == QDBusMessage::ErrorMessage)
            return fail("Cannot stop a CLI session in this login: " + reply.errorMessage() +
                ". Use Ctrl+C in its terminal, or the owning GUI's Disconnect button for GUI sessions.");
        out << "Stop requested.\n";
        return 0;
    }
    auto checked = [&fail](const QDBusMessage& reply) {
        if (reply.type() != QDBusMessage::ErrorMessage) return true;
        fail(reply.errorName() + ": " + reply.errorMessage());
        return false;
    };
    if (action == "list")
    {
        const QDBusReply<QVariantList> reply = Call(systemBus, "SavedGamePads");
        if (!reply.isValid()) return fail(reply.error().message());
        out << "Saved GamePads:\n";
        for (const auto& item : reply.value())
        {
            const auto pad = qdbus_cast<QVariantMap>(item);
            out << "  " << pad.value("mac").toString() << "  " << pad.value("name").toString() << '\n';
        }
        if (reply.value().isEmpty()) out << "  None paired. Use --pair.\n";
        return 0;
    }
    if (action == "status" || action == "diagnostics")
    {
        const QDBusReply<QVariantMap> reply = Call(systemBus, action == "status" ? "GetStatus" : "GetDiagnostics");
        if (!reply.isValid()) return fail(reply.error().message());
        if (action == "status") PrintStatus(reply.value(), out);
        else out << reply.value().value("report").toString();
        return 0;
    }
    if (action == "prepare") return checked(Call(systemBus, "PrepareSystem")) ? 0 : 1;

    SessionControl control;
    auto sessionBus = QDBusConnection::sessionBus();
    if (!sessionBus.registerService(CliService))
        return fail("Another CLI session is running, or the desktop session bus is unavailable. Use --stop first.");
    if (!sessionBus.registerObject(CliPath, &control, QDBusConnection::ExportAllSlots))
        return fail("Cannot register CLI session controls.");
    interrupted = 0;
    std::signal(SIGINT, Interrupt);
    std::signal(SIGTERM, Interrupt);
    out << "Taking over " << interface << " for the GamePad. Polkit may request authorization.\n"
        << "Keep this terminal open. Ctrl+C or barista --stop ends the session.\n" << Qt::flush;
    QVariantList arguments{interface};
    if (action == "pair") arguments << code;
    arguments << parser.value("mode") << country;
    if (!checked(Call(systemBus, action == "pair" ? "PairWithCountry" : "StartSessionWithCountry", arguments)))
        return 1;
    int result = 0;
    QString lastProgress;
    bool pairingShown = false;
    while (!interrupted && !control.stopping)
    {
        const QDBusReply<QVariantMap> reply = Call(systemBus, "GetStatus");
        if (!reply.isValid()) { result = fail(reply.error().message()); break; }
        const auto status = reply.value();
        const auto phase = status.value("phase").toString();
        const auto progress = phase + '/' + status.value("pairingStep").toString() + '/' +
            (status.value("connected").toBool() ? "connected" : "disconnected");
        if (!status.value("error").toString().isEmpty() || phase == "failed")
        {
            PrintStatus(status, error);
            result = 1;
            break;
        }
        if (progress != lastProgress) { PrintStatus(status, out); lastProgress = progress; }
        if (action == "pair" && phase == "pairing" && !pairingShown && !status.value("busy").toBool())
        {
            const QStringList symbols{"Spade", "Heart", "Diamond", "Club"};
            out << "Pair now: press SYNC on the GamePad and enter: ";
            for (const auto digit : code) out << symbols[digit.digitValue()] << "  ";
            out << '\n' << Qt::flush;
            pairingShown = true;
        }
        if (!status.value("running").toBool() && !status.value("busy").toBool()) break;
        Wait();
    }
    out << "Stopping session...\n" << Qt::flush;
    if (!checked(Call(systemBus, "StopSession"))) result = 1;
    // The service also tears down the worker if this D-Bus owner disappears.
    return result;
#endif
}
}

#ifdef BARISTA_LINUX_CONTROL
#include "cli.moc"
#endif
