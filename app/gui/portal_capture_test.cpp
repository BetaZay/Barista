#include "portal_capture.h"
#include <QCoreApplication>
#include <QDBusContext>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDBusUnixFileDescriptor>
#include <QDBusObjectPath>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>
#include <stdexcept>

struct PortalStream
{
    uint node;
    QVariantMap properties;
};
Q_DECLARE_METATYPE(PortalStream)
QDBusArgument &operator<<(QDBusArgument &argument, const PortalStream &stream)
{
    argument.beginStructure();
    argument << stream.node << stream.properties;
    argument.endStructure();
    return argument;
}
const QDBusArgument &operator>>(const QDBusArgument &argument, PortalStream &stream)
{
    argument.beginStructure();
    argument >> stream.node >> stream.properties;
    argument.endStructure();
    return argument;
}

class Request : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Request")
  public:
    using QObject::QObject;
  signals:
    void Response(uint code, const QVariantMap &results);
  public slots:
    void Close()
    {
    }
};
class Session : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Session")
  public:
    int closes = 0;
  public slots:
    void Close()
    {
        ++closes;
    }
  signals:
    void Closed();
};
class Portal : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.ScreenCast")
  public:
    int creates = 0, selections = 0, starts = 0, remotes = 0;
    bool validOptions = true;
    Session session;
    QDBusObjectPath Reply(const QVariantMap &options, uint code, const QVariantMap &results)
    {
        auto bus = QDBusConnection::sessionBus();
        auto sender = message().service().mid(1);
        sender.replace('.', '_');
        const QString path = "/org/freedesktop/portal/desktop/request/" + sender + '/' +
                             options.value("handle_token").toString();
        auto *request = new Request(this);
        if (!bus.registerObject(
                path, request, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals))
            throw std::runtime_error("cannot register request");
        QTimer::singleShot(5, request,
                           [request, code, results] { emit request->Response(code, results); });
        return QDBusObjectPath(path);
    }
  public slots:
    QDBusObjectPath CreateSession(const QVariantMap &options)
    {
        ++creates;
        return Reply(options, 0,
                     {{"session_handle", QString("/org/freedesktop/portal/desktop/session/test")}});
    }
    QDBusObjectPath SelectSources(const QDBusObjectPath &, const QVariantMap &options)
    {
        ++selections;
        validOptions &= options.value("types").toUInt() == 3 &&
                        !options.value("multiple").toBool() &&
                        options.value("cursor_mode").toUInt() == 2;
        return Reply(options, 0, {});
    }
    QDBusObjectPath Start(const QDBusObjectPath &, const QString &, const QVariantMap &options)
    {
        ++starts;
        if (starts <= 2)
            return Reply(options, 1, {}); // Cancellation releases the session.
        return Reply(options, 0, {{"streams", QVariant::fromValue(QList<PortalStream>{{7, {}}})}});
    }
    QDBusUnixFileDescriptor OpenPipeWireRemote(const QDBusObjectPath &, const QVariantMap &)
    {
        ++remotes;
        sendErrorReply(QDBusError::Failed, "No real PipeWire stream in this fixture");
        return {};
    }
};
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // Opt-in workstation check; CTest always uses the isolated mock portal below.
    if (app.arguments().contains("--live"))
    {
        PortalCapture capture;
        QObject::connect(&capture, &PortalCapture::Frame, &app,
                         [&](const QImage &image)
                         {
                             std::printf("Captured desktop frame: %d x %d\n", image.width(),
                                         image.height());
                             capture.Stop();
                             QTimer::singleShot(100, &app, [&] { app.exit(0); });
                         });
        QObject::connect(&capture, &PortalCapture::Error, &app,
                         [&](const QString &error)
                         {
                             std::fprintf(stderr, "%s\n", error.toUtf8().constData());
                             app.exit(1);
                         });
        QTimer::singleShot(60000, &app,
                           [&]
                           {
                               capture.Stop();
                               app.exit(2);
                           });
        capture.Start();
        return app.exec();
    }
    auto bus = QDBusConnection::sessionBus();
    qDBusRegisterMetaType<PortalStream>();
    qDBusRegisterMetaType<QList<PortalStream>>();
    Portal portal;
    if (!bus.registerService("org.freedesktop.portal.Desktop") ||
        !bus.registerObject("/org/freedesktop/portal/desktop", &portal,
                            QDBusConnection::ExportAllSlots) ||
        !bus.registerObject("/org/freedesktop/portal/desktop/session/test", &portal.session,
                            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals))
        return 1;
    PortalCapture capture;
    int errors = 0;
    for (int trial = 0; trial < 3; ++trial)
    {
        QEventLoop loop;
        const auto connection =
            QObject::connect(&capture, &PortalCapture::Error, &loop,
                             [&](const QString &)
                             {
                                 ++errors;
                                 QTimer::singleShot(25, &loop, &QEventLoop::quit);
                             });
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        capture.Start();
        loop.exec();
        QObject::disconnect(connection);
        if (errors != trial + 1 || portal.session.closes != trial + 1)
            return 2;
    }
    return portal.validOptions && portal.creates == 3 && portal.selections == 3 &&
                   portal.starts == 3 && portal.remotes == 1
               ? 0
               : 3;
}
#include "portal_capture_test.moc"
