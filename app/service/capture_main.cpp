#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QFile>
#include <QProcess>
#include <QTextStream>
#include <QVariantMap>
#include <unistd.h>
#include <csignal>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void Interrupt(int) { interrupted = 1; }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::signal(SIGINT, Interrupt);
    std::signal(SIGTERM, Interrupt);
    QCoreApplication::setApplicationName("barista-capture");
    QCommandLineParser parser;
    parser.setApplicationDescription("Capture and analyze a GamePad test through Polkit. Keep the GamePad off until READY.");
    parser.addHelpOption();
    parser.addOption({"ap", "Barista AP adapter", "interface", "wlan0"});
    parser.addOption({"radio", "Independent capture adapter", "interface", "wlan1"});
    parser.addOption({"seconds", "Recording time after GamePad traffic begins (10..300)", "seconds", "60"});
    parser.process(app);
    QTextStream out(stdout), error(stderr);
    if (geteuid() == 0)
    {
        error << "Run barista-capture as your desktop user. Polkit authorizes the installed helper.\n";
        return 1;
    }
    bool valid = false;
    const int seconds = parser.value("seconds").toInt(&valid);
    if (!valid || seconds < 10 || seconds > 300)
    {
        error << "Choose 10..300 seconds.\n";
        return 1;
    }
    auto bus = QDBusConnection::systemBus();
    auto call = [&bus](const QString& method, const QList<QVariant>& arguments = {}) {
        auto message = QDBusMessage::createMethodCall("org.barista.Service1", "/org/barista/Service1",
                                                      "org.barista.Service1", method);
        message.setArguments(arguments);
        return bus.call(message, QDBus::Block, 150000);
    };
    QDBusReply<QVariantMap> status = call("GetStatus");
    if (!status.isValid())
    {
        error << status.error().message() << '\n';
        return 1;
    }
    const bool started = !status.value().value("running").toBool();
    if (!started && (status.value().value("interface").toString() != parser.value("ap") ||
                     status.value().value("mode").toString() != "real"))
    {
        error << "Current session must use the selected AP adapter in real mode.\n";
        return 1;
    }
    if (started)
    {
        out << "Starting Barista. Keep the GamePad off until capture says READY.\n" << Qt::flush;
        auto reply = call("StartSession", {parser.value("ap"), "real"});
        if (reply.type() == QDBusMessage::ErrorMessage)
        {
            error << reply.errorMessage() << '\n';
            return 1;
        }
    }
    else
    {
        out << "Attaching to the existing session. Power-cycle the GamePad after READY to capture its handshake.\n" << Qt::flush;
    }
    QProcess capture;
    capture.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    QByteArray transcript;
    auto readOutput = [&] {
        const auto data = capture.readAllStandardOutput();
        transcript += data;
        out << QString::fromUtf8(data) << Qt::flush;
    };
    capture.start(BARISTA_PKEXEC, {BARISTA_CAPTURE_HELPER, "--ap", parser.value("ap"),
                                 "--radio", parser.value("radio"), "--seconds", QString::number(seconds)});
    int result = 1;
    if (!capture.waitForStarted(10000))
        error << "Cannot launch the installed capture helper: " << capture.errorString() << '\n';
    else
    {
        while (!capture.waitForFinished(250) && !interrupted)
        {
            readOutput();
            QCoreApplication::processEvents();
        }
        readOutput();
        if (interrupted)
            result = 130;
        else if (capture.exitStatus() == QProcess::NormalExit)
            result = capture.exitCode();
    }
    QDBusReply<QVariantMap> diagnostics = call("GetDiagnostics");
    if (diagnostics.isValid())
    {
        const auto report = diagnostics.value().value("report").toString();
        for (const auto& line : transcript.split('\n'))
        {
            if (!line.startsWith("RESULT: /var/lib/barista-captures/")) continue;
            const auto directory = QString::fromUtf8(line.mid(8));
            QFile file(directory + "/service-diagnostics.txt");
            if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            {
                file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
                file.write(report.toUtf8());
            }
        }
    }
    if (started)
    {
        auto reply = call("StopSession");
        if (reply.type() == QDBusMessage::ErrorMessage)
        {
            error << "Session cleanup failed: " << reply.errorMessage() << '\n';
            result = 1;
        }
    }
    // The helper's parent-death signal handles cleanup when interrupted.
    if (interrupted)
        _exit(result);
    return result;
}
