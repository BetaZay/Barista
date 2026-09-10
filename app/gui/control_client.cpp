#include "control_client.h"
#include "api/diagnostics.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QSysInfo>
#include <QTimer>
#include <QVariantMap>
#ifdef BARISTA_LINUX_CONTROL
#include <QDBusConnection>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif

namespace
{
#ifdef BARISTA_LINUX_CONTROL
constexpr auto SupportLogDirectory = "/var/log/barista/support";

QStringList LocalSupportLogFiles()
{
    return QDir(SupportLogDirectory).entryList({"*.log"}, QDir::Files, QDir::Time);
}

QString UnavailableSupportReport()
{
    const auto advice = barista::api::AdviceForDiagnostic("SERVICE_UNAVAILABLE");
    return QString("Barista support report\nschema_version=1\ngenerated_utc=%1\nbarista_version=%2\nplatform=%3\nkernel=%4 %5\ndiagnostic_code=SERVICE_UNAVAILABLE\ndiagnosis=%6\nsuggested_action=%7\n\nPrivacy\nThis report excludes network addresses, pairing codes, credentials, usernames, and raw service logs.\n")
        .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs), QCoreApplication::applicationVersion(),
            QSysInfo::prettyProductName(), QSysInfo::kernelType(), QSysInfo::kernelVersion(),
            QString::fromUtf8(advice.summary.data(),static_cast<qsizetype>(advice.summary.size())),
            QString::fromUtf8(advice.action.data(),static_cast<qsizetype>(advice.action.size())));
}
#endif

barista::api::SessionStatus UnavailableStatus(const QString& message)
{
    barista::api::SessionStatus status;
    status.error = barista::api::Error{
        .code = barista::api::ErrorCode::Unavailable,
        .message = message.toStdString(),
    };
    return status;
}

barista::api::SessionStatus DecodeStatus(const QVariantMap& value)
{
    barista::api::SessionStatus status;
    status.apiVersion = value.value("apiVersion", barista::api::ApiVersion).toUInt();
    status.available = value.value("available").toBool();
    status.activating = value.value("activating").toBool();
    status.platform = value.value("platform").toString().toStdString();
    status.phase = barista::api::ParseSessionPhase(value.value("phase").toString().toStdString())
        .value_or(barista::api::SessionPhase::Failed);
    status.mode = barista::api::ParseSessionMode(value.value("mode").toString().toStdString());
    status.running = value.value("running").toBool();
    status.gamePadConnected = value.value("connected").toBool();
    if (value.value("batteryAvailable").toBool())
        status.batteryPercent = static_cast<uint8_t>(value.value("battery").toUInt());
    status.interfaceName = value.value("interface").toString().toStdString();
    status.ownedByCaller = value.value("ownedByCaller").toBool();
    status.busy = value.value("busy").toBool();
    status.mediaEndpoint = value.value("mediaEndpoint").toString().toStdString();
    status.application.connected = value.value("appConnected").toBool();
    status.application.name = value.value("appName").toString().toStdString();
    status.application.pid = value.value("appPid").toUInt();
    status.application.lastSeen = value.value("appLastSeen").toULongLong();
    status.application.connectedAt = value.value("appConnectedAt").toULongLong();
    status.application.idleLogo = value.value("appIdleLogo").toString().toStdString();
    status.capabilities.controller = value.value("controllerSupported").toBool();
    status.capabilities.pairing = value.value("pairingSupported").toBool();
    status.capabilities.systemPreparation = value.value("setupSupported").toBool();
    status.capabilities.mediaStreaming = true;
    status.capabilities.controllerSetup = value.value("controllerSetupAvailable").toBool();
    status.health.networkManagerRunning = value.value("networkManagerRunning").toBool();
    status.health.authorizationRunning = value.value("polkitRunning").toBool();
    status.health.engineInstalled = value.value("engineInstalled").toBool();
    status.health.hostapdInstalled = value.value("hostapdInstalled").toBool();
    status.health.authorizationInstalled = value.value("authorizationInstalled").toBool();
    status.health.legacySessionPresent = value.value("legacySessionPresent").toBool();
    for (const auto& tool : value.value("missingTools").toStringList())
        status.health.missingTools.push_back(tool.toStdString());
    const auto error = value.value("error").toString();
    if (!error.isEmpty())
        status.error = barista::api::Error{.code = barista::api::ErrorCode::Failed, .message = error.toStdString(),
            .diagnosticCode = value.value("errorCode").toString().toStdString(),
            .action = value.value("errorAction").toString().toStdString()};
    return status;
}
}

void ControlClient::Refresh() { if (!m_pollPending && QDateTime::currentMSecsSinceEpoch() >= m_nextRetry) Call("GetStatus"); }
void ControlClient::Retry() { m_nextRetry = 0; Refresh(); }
void ControlClient::Prepare() { Call("PrepareSystem"); }
void ControlClient::Start(const barista::api::StartSessionRequest& request)
{
    Call("StartSessionWithCountry", {QString::fromStdString(request.interfaceName),
        QString::fromLatin1(barista::api::SessionModeName(request.mode)),
        QString::fromStdString(request.regulatoryCountry)});
}
void ControlClient::Pair(const barista::api::PairRequest& request)
{
    Call("PairWithCountry", {QString::fromStdString(request.interfaceName),
        QString::fromStdString(barista::api::PairCodeName(request.code)),
        QString::fromLatin1(barista::api::SessionModeName(request.mode)),
        QString::fromStdString(request.regulatoryCountry)});
}
void ControlClient::RenameGamePad(const barista::api::RenameGamePadRequest& request)
{
    Call("RenameGamePad", {QString::fromStdString(request.mac), QString::fromStdString(request.name)});
}
void ControlClient::RemoveGamePad(const barista::api::RemoveGamePadRequest& request)
{
    Call("RemoveGamePad", {QString::fromStdString(request.mac)});
}
void ControlClient::RefreshGamePads()
{
#ifdef BARISTA_LINUX_CONTROL
    auto message = QDBusMessage::createMethodCall(
        "org.barista.Service1", "/org/barista/Service1", "org.barista.Service1", "SavedGamePads");
    message.setAutoStartService(true);
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 25000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](auto* completed) {
        std::vector<barista::api::GamePad> gamePads;
        const QDBusPendingReply<QVariantList> reply = *completed;
        if (!reply.isError())
        {
            for (const auto& value : reply.value())
            {
                QVariantMap record = value.toMap();
                if (record.isEmpty() && value.canConvert<QDBusArgument>())
                {
                    auto argument = qvariant_cast<QDBusArgument>(value);
                    argument >> record;
                }
                const auto mac = record.value("mac").toString();
                if (!mac.isEmpty())
                    gamePads.push_back({mac.toStdString(), record.value("name").toString().toStdString()});
            }
            emit GamePads(gamePads);
        }
        completed->deleteLater();
    });
#else
    emit GamePads({});
#endif
}
void ControlClient::RefreshDiagnostics()
{
#ifdef BARISTA_LINUX_CONTROL
    auto message = QDBusMessage::createMethodCall(
        "org.barista.Service1", "/org/barista/Service1", "org.barista.Service1", "GetDiagnostics");
    message.setAutoStartService(true);
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 25000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](auto* completed) {
        const QDBusPendingReply<QVariantMap> reply = *completed;
        if (!reply.isError())
            emit Diagnostics(reply.value().value("report").toString(),
                reply.value().value("logDirectory").toString(), reply.value().value("logFiles").toStringList(),
                reply.value().value("sessionId").toString());
        else
        {
            const QString directory = QDir(SupportLogDirectory).exists() ? QString::fromLatin1(SupportLogDirectory) : QString();
            emit Diagnostics(UnavailableSupportReport(), directory, LocalSupportLogFiles(), {});
        }
        completed->deleteLater();
    });
#else
    emit Diagnostics({}, {}, {}, {});
#endif
}
void ControlClient::Stop()
{
    if (m_operationPending) { m_stopAfterOperation = true; return; }
    Call("StopSession");
}
void ControlClient::Call(const QString& method, const QVariantList& arguments)
{
#ifdef BARISTA_LINUX_CONTROL
    const bool poll = method == "GetStatus";
    if (!poll && m_operationPending) return;
    if (poll) m_pollPending = true;
    else { m_operationPending = true; emit Pending(true); }
    auto message = QDBusMessage::createMethodCall("org.barista.Service1","/org/barista/Service1","org.barista.Service1",method);
    message.setArguments(arguments);
    // The installed activation file starts the idle system service on demand.
    // Only hardware/setup mutations prompt for polkit authorization.
    message.setAutoStartService(true);
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message,poll ? 25000 : 150000),this);
    connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,poll,method](auto* completed) {
        if (poll) {
            m_pollPending = false;
            QDBusPendingReply<QVariantMap> reply = *completed;
            if (reply.isError()) {
                m_nextRetry = QDateTime::currentMSecsSinceEpoch() + 5000;
                emit Status(UnavailableStatus("Automatic service startup failed. Check the Barista installation and system D-Bus. " + reply.error().message()));
            } else {
                m_nextRetry = 0;
                auto value = reply.value();
                value.insert("available", true);
                emit Status(DecodeStatus(value));
            }
        } else {
            m_operationPending = false; emit Pending(false);
            QDBusPendingReply<> reply = *completed;
            const bool success = !reply.isError();
            if (!success) emit Error(reply.error().message());
            if (method == "StopSession") emit Stopped(success);
            if (success && (method == "RenameGamePad" || method == "RemoveGamePad"))
                RefreshGamePads();
            if (m_stopAfterOperation) {
                m_stopAfterOperation = false;
                QTimer::singleShot(0, this, &ControlClient::Stop);
            }
            Refresh();
        }
        completed->deleteLater();
    });
#else
    Q_UNUSED(method); Q_UNUSED(arguments);
    emit Status(UnavailableStatus("No radio/service backend is implemented on this platform yet."));
#endif
}
