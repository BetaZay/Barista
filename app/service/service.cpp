#include "service.h"
#include "../branding/idle_screen.h"
#include "api/diagnostics.h"
#include "api/controller.h"
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QSysInfo>
#include <QTextStream>
#include <QUuid>
#include <QSaveFile>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <algorithm>

namespace {
constexpr auto ControlSocket = "/run/barista/worker.sock";
constexpr auto CredentialsFile = "/var/lib/drcd/credentials.conf";
constexpr auto SupportLogDirectory = "/var/log/barista/support";
constexpr auto RawLogDirectory = "/var/log/barista/private";
constexpr int GamePadBatteryFull = 176;

QString ModeName(barista::api::SessionMode mode)
{
    return QString::fromLatin1(barista::api::SessionModeName(mode));
}

int BatteryPercent(int raw)
{
    return qBound(0, (raw * 100 + GamePadBatteryFull / 2) / GamePadBatteryFull, 100);
}

QString DiagnosticCodeForError(const QString& message)
{
    if (message.contains("Authorization", Qt::CaseInsensitive) ||
        message.contains("denied", Qt::CaseInsensitive)) return "AUTHORIZATION_DENIED";
    if (message.contains("uinput", Qt::CaseInsensitive) ||
        message.contains("virtual controller", Qt::CaseInsensitive)) return "CONTROLLER_UNAVAILABLE";
    if (message.contains("system helper", Qt::CaseInsensitive) ||
        message.contains("System preparation", Qt::CaseInsensitive) ||
        message.contains("NetworkManager", Qt::CaseInsensitive)) return "SYSTEM_PREPARATION_FAILED";
    return QString::fromLatin1(barista::api::ClassifyDiagnosticMessage(message.toStdString()));
}

bool OpenLogFile(QFile& file, const QString& path)
{
    if (file.isOpen()) file.close();
    file.setFileName(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    if (::chmod(path.toLocal8Bit().constData(), 0644) == 0) return true;
    file.close();
    QFile::remove(path);
    return false;
}

void AppendPublicLog(QFile& file, const QByteArray& data)
{
    constexpr qint64 MaximumLogBytes = 2 * 1024 * 1024;
    if (!file.isOpen() || file.size() + data.size() > MaximumLogBytes) return;
    file.write(data);
    file.flush();
}
}

std::vector<barista::api::GamePad> Service::GamePads() const
{
    std::vector<barista::api::GamePad> result;
    QFile file(CredentialsFile);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return result;
    QHash<QString,QString> names;
    QStringList macs;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.startsWith("gamepad_name_")) names.insert(line.mid(13,line.indexOf('=') - 13),line.mid(line.indexOf('=') + 1));
        else if (line.startsWith("gamepad_mac=")) macs.append(line.mid(12));
    }
    macs.removeDuplicates();
    for (const auto& mac : macs)
        result.push_back({mac.toStdString(), names.value(mac).toStdString()});
    return result;
}

QVariantList Service::SavedGamePads()
{
    QVariantList result;
    for (const auto& gamePad : GamePads())
        result.append(QVariantMap{{"mac",QString::fromStdString(gamePad.mac)},
            {"name",QString::fromStdString(gamePad.name)}});
    return result;
}

void Service::RenameGamePad(const QString& mac, const QString& name)
{
    RenameGamePadRecord({mac.toStdString(), name.toStdString()});
}

void Service::RenameGamePadRecord(const barista::api::RenameGamePadRequest& request)
{
    const QString mac = QString::fromStdString(request.mac);
    const QString name = QString::fromStdString(request.name);
    QFile input(CredentialsFile);
    if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QStringList lines = QString::fromUtf8(input.readAll()).split('\n',Qt::SkipEmptyParts);
    bool found = false;
    for (auto& line : lines) if (line.startsWith("gamepad_name_" + mac + "=")) { line = "gamepad_name_" + mac + "=" + name; found = true; }
    if (!found) lines.append("gamepad_name_" + mac + "=" + name);
    QSaveFile output(CredentialsFile); if (!output.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    output.write((lines.join('\n') + '\n').toUtf8()); output.commit();
}

void Service::RemoveGamePad(const QString& mac)
{
    RemoveGamePadRecord({mac.toStdString()});
}

void Service::RemoveGamePadRecord(const barista::api::RemoveGamePadRequest& request)
{
    const QString mac = QString::fromStdString(request.mac);
    QFile input(CredentialsFile);
    if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QStringList lines = QString::fromUtf8(input.readAll()).split('\n',Qt::SkipEmptyParts);
    lines.erase(std::remove_if(lines.begin(),lines.end(),[&](const QString& line) { return line == "gamepad_mac=" + mac || line.startsWith("gamepad_name_" + mac + "="); }),lines.end());
    QSaveFile output(CredentialsFile); if (!output.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    output.write((lines.join('\n') + '\n').toUtf8()); output.commit();
}

namespace {
bool BusServiceRunning(const QString& name)
{
    const QDBusReply<bool> reply = QDBusConnection::systemBus().interface()->isServiceRegistered(name);
    return reply.isValid() && reply.value();
}
bool TrustedExecutable(const QString& path)
{
    if (!QFileInfo(path).isExecutable()) return false;
    QString current = QFileInfo(path).absoluteFilePath();
    if (current.isEmpty()) return false;
    do {
        struct stat info{};
        if (lstat(current.toLocal8Bit().constData(), &info) || S_ISLNK(info.st_mode) ||
            info.st_uid != 0 || (info.st_mode & 0022)) return false;
        current = QFileInfo(current).absolutePath();
    } while (current != "/");
    return true;
}
bool TrustedSystemHelper(const QString& path)
{
    // modprobe is commonly a root-owned symlink to kmod. Keep its argv[0]
    // for command dispatch, but validate both the link location and target.
    struct stat info{};
    if (lstat(path.toLocal8Bit().constData(), &info)) return false;
    if (!S_ISLNK(info.st_mode)) return TrustedExecutable(path);
    return info.st_uid == 0 && TrustedExecutable(QFileInfo(path).absolutePath()) &&
        TrustedExecutable(QFileInfo(path).canonicalFilePath());
}
}
Service::Service(QObject* parent) : QObject(parent)
{
    QDir().mkpath(SupportLogDirectory);
    QDir().mkpath(RawLogDirectory);
    (void)::chmod("/var/log/barista", 0755);
    (void)::chmod(SupportLogDirectory, 0755);
    (void)::chmod(RawLogDirectory, 0700);
    PruneSupportLogs();
    m_worker.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_worker, &QProcess::started, this, [this] {
        RecordDiagnostic("SESSION_STARTED");
        const QDBusReply<bool> alive = QDBusConnection::systemBus().interface()->isServiceRegistered(m_owner);
        if (m_stopping || !alive.isValid() || !alive.value()) StopWorker();
    });
    connect(&m_worker, &QProcess::readyReadStandardOutput, this, &Service::ProcessWorkerOutput);
    connect(&m_worker, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        m_error = m_worker.errorString();
        m_errorCode = "ENGINE_START_FAILED";
        RecordDiagnostic(m_errorCode, "service", QString("process_error=%1").arg(static_cast<int>(error)));
        if (error == QProcess::FailedToStart) {
            m_controller.Stop(); m_input.reset(); m_owner.clear(); m_phase = "idle"; m_connected = false;
            CloseSupportRun();
        }
    });
    connect(&m_worker, qOverload<int,QProcess::ExitStatus>(&QProcess::finished), this,
        [this](int code, QProcess::ExitStatus exitStatus) {
        ProcessWorkerOutput();
        m_controller.Stop(); m_input.reset(); m_connected = false; m_phase = "idle";
        if (!m_stopping) {
            m_error = QString("Radio engine exited unexpectedly (%1)").arg(code);
            m_errorCode = "ENGINE_EXITED";
            RecordDiagnostic(m_errorCode, "engine", QString("exit_code=%1 exit_status=%2").arg(code)
                .arg(exitStatus == QProcess::CrashExit ? "crash" : "normal"));
        }
        m_stopping = false; m_owner.clear(); m_endpoint.clear();
        CloseSupportRun();
    });
    auto* watcher = new QDBusServiceWatcher(this);
    watcher->setConnection(QDBusConnection::systemBus());
    watcher->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this,watcher](const QString& name) {
        if (name == m_owner) StopWorker();
        watcher->removeWatchedService(name);
    });
    // A single watcher is located by type when a new authenticated owner is assigned.
    connect(&m_poll, &QTimer::timeout, this, &Service::Poll);
    m_poll.start(500);
    m_statusTimeout.setSingleShot(true);
    connect(&m_statusTimeout, &QTimer::timeout, &m_statusSocket, &QLocalSocket::abort);
    connect(&m_statusSocket, &QLocalSocket::connected, this, [this] { m_statusSocket.write("status\n"); });
    connect(&m_statusSocket, &QLocalSocket::readyRead, this, [this] {
        m_response += m_statusSocket.readAll();
        if (m_response.size() > 16384) m_statusSocket.abort();
    });
    connect(&m_statusSocket, &QLocalSocket::disconnected, this, &Service::ParseStatus);
    connect(&m_inputTimer, &QTimer::timeout, this, [this] {
        if (!m_input) return;
        std::array<uint8_t,128> raw{};
        const auto state = m_input->read_input(raw) ? barista::DecodeInput(raw) : barista::ControllerState{};
        if (!m_controller.Submit(state)) {
            m_error = "Virtual controller write failed; session stopped";
            m_errorCode = "CONTROLLER_WRITE_FAILED";
            RecordDiagnostic(m_errorCode, "controller");
            StopWorker();
        }
    });
    m_inputTimer.start(8);
}
Service::~Service()
{
    StopWorker();
    // The service unit's timeout handles an unresponsive engine; allow normal cleanup.
    if (m_worker.state() != QProcess::NotRunning) m_worker.waitForFinished(15000);
}
barista::api::SessionStatus Service::Status(bool ownedByCaller) const
{
    barista::api::SessionStatus status;
    status.available = true;
    status.platform = "linux";
    status.phase = barista::api::ParseSessionPhase(m_phase.toStdString())
        .value_or(barista::api::SessionPhase::Failed);
    status.mode = m_mode;
    status.running = m_worker.state() != QProcess::NotRunning;
    status.gamePadConnected = m_connected;
    if (m_batteryAvailable)
        status.batteryPercent = static_cast<uint8_t>(m_battery);
    status.interfaceName = m_interface.toStdString();
    status.ownedByCaller = ownedByCaller;
    status.busy = m_authorizing || m_stopping;
    if (!m_error.isEmpty())
    {
        const QString diagnosticCode = m_errorCode.isEmpty() ? DiagnosticCodeForError(m_error) : m_errorCode;
        const auto advice = barista::api::AdviceForDiagnostic(diagnosticCode.toStdString());
        status.error = barista::api::Error{
            .code = barista::api::ErrorCode::Failed,
            .message = m_error.toStdString(),
            .diagnosticCode = diagnosticCode.toStdString(),
            .action = std::string(advice.action),
        };
    }
    if (ownedByCaller && m_mode == barista::api::SessionMode::Real)
        status.mediaEndpoint = m_endpoint.toStdString();

    for (const auto* tool : {"iw", "ip", "nmcli"})
    {
        if (QStandardPaths::findExecutable(tool,{"/usr/sbin","/usr/bin","/sbin","/bin"}).isEmpty())
            status.health.missingTools.emplace_back(tool);
    }
    if (m_mode == barista::api::SessionMode::Real && !m_endpoint.isEmpty())
    {
        barista::api::AppHook::ConnectedAppInfo appInfo{};
        if (barista::api::AppHook::read_app_lock(m_endpoint.toStdString(), appInfo))
        {
            status.application.connected = appInfo.connected;
            status.application.name = std::move(appInfo.name);
            status.application.pid = appInfo.pid;
            status.application.lastSeen = appInfo.last_seen;
            status.application.connectedAt = appInfo.connected_at;
            status.application.idleLogo = std::move(appInfo.idle_logo);
        }
    }
    status.capabilities.controller = QFileInfo::exists("/dev/uinput");
    status.capabilities.pairing = true;
    status.capabilities.systemPreparation = true;
    status.capabilities.mediaStreaming = true;
    status.capabilities.controllerSetup = TrustedSystemHelper(BARISTA_MODPROBE);
    status.health.networkManagerRunning = BusServiceRunning("org.freedesktop.NetworkManager");
    status.health.authorizationRunning = BusServiceRunning("org.freedesktop.PolicyKit1");
    status.health.engineInstalled = TrustedExecutable(BARISTA_WORKER);
    status.health.hostapdInstalled = TrustedExecutable(BARISTA_HOSTAPD);
    status.health.authorizationInstalled = TrustedExecutable(BARISTA_PKCHECK);
    status.health.legacySessionPresent = QFileInfo::exists("/tmp/drcd.sock");
    return status;
}

QVariantMap Service::GetStatus()
{
    const bool mine = calledFromDBus() && message().service() == m_owner;
    const auto status = Status(mine);
    QStringList missingTools;
    for (const auto& tool : status.health.missingTools)
        missingTools.push_back(QString::fromStdString(tool));
    const QString error = status.error ? QString::fromStdString(status.error->message) : QString();
    const QString diagnosticCode = status.error ? QString::fromStdString(status.error->diagnosticCode) : QString();
    const QString diagnosticAction = status.error ? QString::fromStdString(status.error->action) : QString();
    return {{"apiVersion",status.apiVersion}, {"platform",QString::fromStdString(status.platform)},
        {"running",status.running}, {"phase",QString::fromLatin1(barista::api::SessionPhaseName(status.phase))},
        {"connected",status.gamePadConnected}, {"mode",status.mode ? ModeName(*status.mode) : QString()},
        {"interface",QString::fromStdString(status.interfaceName)},
        {"batteryAvailable",status.batteryPercent.has_value()}, {"battery",status.batteryPercent.value_or(0)},
        {"ownedByCaller",status.ownedByCaller}, {"busy",status.busy}, {"error",error},
        {"errorCode",diagnosticCode}, {"errorAction",diagnosticAction},
        {"mediaEndpoint",QString::fromStdString(status.mediaEndpoint)},
        {"appConnected",status.application.connected}, {"appName",QString::fromStdString(status.application.name)},
        {"appPid",status.application.pid}, {"appLastSeen",qulonglong(status.application.lastSeen)},
        {"appConnectedAt",qulonglong(status.application.connectedAt)},
        {"appIdleLogo",QString::fromStdString(status.application.idleLogo)},
        {"controllerSupported",status.capabilities.controller}, {"pairingSupported",status.capabilities.pairing},
        {"setupSupported",status.capabilities.systemPreparation},
        {"controllerSetupAvailable",status.capabilities.controllerSetup},
        {"networkManagerRunning",status.health.networkManagerRunning},
        {"polkitRunning",status.health.authorizationRunning},
        {"engineInstalled",status.health.engineInstalled}, {"hostapdInstalled",status.health.hostapdInstalled},
        {"authorizationInstalled",status.health.authorizationInstalled},
        {"missingTools",missingTools}, {"legacySessionPresent",status.health.legacySessionPresent}};
}

QVariantMap Service::GetDiagnostics()
{
    const auto files = SupportLogFiles();
    return {{"schemaVersion",1}, {"report",BuildSupportReport()},
        {"logDirectory",QString::fromLatin1(SupportLogDirectory)}, {"logFiles",files},
        {"latestLog",files.isEmpty() ? QString() : files.front()}, {"sessionId",m_sessionId}};
}

void Service::StartSupportRun(const QString& mode)
{
    CloseSupportRun();
    PruneSupportLogs();
    m_diagnosticEvents.clear();
    m_latestMediaTiming.clear(); m_latestTransportStats.clear();
    m_lastMediaTimingLog = 0; m_lastTransportStatsLog = 0;
    m_pairingCycle = 0;
    m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString stamp = QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss");
    m_runLogName = QString("%1-%2-%3.log").arg(mode == "maintenance" ? "maintenance" : "run",
        stamp, m_sessionId.left(8));
    if (!OpenLogFile(m_runLog, QString::fromLatin1(SupportLogDirectory) + '/' + m_runLogName)) return;
    const QString header = QString("Barista support log\nversion=%1\nsession_id=%2\nstarted_utc=%3\nmode=%4\ninterface=%5\n\n")
        .arg(QString::fromLatin1(BARISTA_VERSION_STRING), m_sessionId,
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs), mode,
            m_interface.isEmpty() ? QString("none") : m_interface);
    AppendPublicLog(m_runLog, header.toUtf8());
}

void Service::RecordDiagnostic(const QString& code, const QString& component, const QString& detail)
{
    if (!barista::api::IsKnownDiagnosticCode(code.toStdString())) return;
    static const QStringList components{"service","engine","wifi","pairing","gamepad","media","controller"};
    const QString safeComponent = components.contains(component) ? component : QString("engine");
    const QString safeDetail = barista::api::IsSafeDiagnosticDetail(detail.toStdString()) ? detail : QString();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (code == "MEDIA_TIMING")
    {
        m_latestMediaTiming = safeDetail;
        if (m_lastMediaTimingLog && now - m_lastMediaTimingLog < 30000) return;
        m_lastMediaTimingLog = now;
    }
    if (code == "MEDIA_TRANSPORT_STATS")
    {
        m_latestTransportStats = safeDetail;
        if (m_lastTransportStatsLog && now - m_lastTransportStatsLog < 30000) return;
        m_lastTransportStatsLog = now;
    }

    if (code == "PAIRING_CYCLE_STARTED")
    {
        if (m_pairingLog.isOpen()) m_pairingLog.close();
        ++m_pairingCycle;
        const QString stamp = QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss");
        const QString name = QString("pairing-%1-%2-cycle-%3.log")
            .arg(stamp, m_sessionId.left(8)).arg(m_pairingCycle, 3, 10, QLatin1Char('0'));
        if (OpenLogFile(m_pairingLog, QString::fromLatin1(SupportLogDirectory) + '/' + name))
        {
            const QString header = QString("Barista pairing-cycle support log\nversion=%1\nsession_id=%2\ncycle=%3\nstarted_utc=%4\ninterface=%5\n\n")
                .arg(QString::fromLatin1(BARISTA_VERSION_STRING), m_sessionId)
                .arg(m_pairingCycle)
                .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs), m_interface);
            AppendPublicLog(m_pairingLog, header.toUtf8());
        }
    }

    const auto advice = barista::api::AdviceForDiagnostic(code.toStdString());
    const QString severity = QString::fromLatin1(barista::api::DiagnosticSeverity(code.toStdString()));
    QString line = QString("%1 [%2] %3 %4: %5 | Next: %6")
        .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs), severity, safeComponent, code,
            QString::fromUtf8(advice.summary.data(), static_cast<qsizetype>(advice.summary.size())),
            QString::fromUtf8(advice.action.data(), static_cast<qsizetype>(advice.action.size())));
    if (!safeDetail.isEmpty()) line += " | Detail: " + safeDetail;
    line += '\n';
    qInfo().noquote() << QString("barista-support[%1]:").arg(m_sessionId.left(8)) << line.trimmed();
    m_diagnosticEvents.push_back(line.trimmed());
    while (m_diagnosticEvents.size() > 200) m_diagnosticEvents.removeFirst();
    AppendPublicLog(m_runLog, line.toUtf8());
    AppendPublicLog(m_pairingLog, line.toUtf8());

    if (severity == "error")
    {
        m_errorCode = code;
        if (m_error.isEmpty())
            m_error = QString::fromUtf8(advice.summary.data(), static_cast<qsizetype>(advice.summary.size()));
    }
    else if (code == "SYSTEM_PREPARATION_SUCCEEDED" || code == "PAIRING_READY" ||
        code == "PAIRING_SUCCEEDED" || code == "RUNTIME_READY" || code == "GAMEPAD_CONNECTED")
    {
        m_error.clear();
        m_errorCode.clear();
    }
    if ((code == "PAIRING_TIMEOUT" || code == "PAIRING_SUCCEEDED" || severity == "error") &&
        m_pairingLog.isOpen())
        m_pairingLog.close();
}

void Service::CloseSupportRun()
{
    if (m_pairingLog.isOpen()) m_pairingLog.close();
    if (m_runLog.isOpen()) m_runLog.close();
}

void Service::ProcessWorkerOutput()
{
    m_workerOutput += m_worker.readAllStandardOutput();
    while (true)
    {
        const qsizetype newline = m_workerOutput.indexOf('\n');
        if (newline < 0) break;
        const QByteArray raw = m_workerOutput.left(newline).trimmed();
        m_workerOutput.remove(0, newline + 1);
        if (raw.isEmpty()) continue;
        if (!raw.startsWith("BARISTA_EVENT|")) {
            qInfo().noquote() << QString("barista-engine[%1]:").arg(m_sessionId.left(8)) << QString::fromUtf8(raw);
            continue;
        }
        const auto fields = raw.split('|');
        if (fields.size() == 4 || fields.size() == 5)
            RecordDiagnostic(QString::fromLatin1(fields[3]), QString::fromLatin1(fields[2]),
                fields.size() == 5 ? QString::fromLatin1(fields[4]) : QString());
    }
    if (m_worker.state() == QProcess::NotRunning && !m_workerOutput.trimmed().isEmpty())
    {
        const QByteArray raw = m_workerOutput.trimmed();
        m_workerOutput.clear();
        if (raw.startsWith("BARISTA_EVENT|"))
        {
            const auto fields = raw.split('|');
            if (fields.size() == 4 || fields.size() == 5)
                RecordDiagnostic(QString::fromLatin1(fields[3]), QString::fromLatin1(fields[2]),
                    fields.size() == 5 ? QString::fromLatin1(fields[4]) : QString());
        }
        else
            qInfo().noquote() << QString("barista-engine[%1]:").arg(m_sessionId.left(8)) << QString::fromUtf8(raw);
    }
    if (m_workerOutput.size() > 64 * 1024)
    {
        qWarning() << "Discarding an overlong radio-engine log line";
        m_workerOutput.clear();
    }
}

QStringList Service::SupportLogFiles() const
{
    QStringList files;
    const auto entries = QDir(SupportLogDirectory).entryInfoList({"*.log"}, QDir::Files, QDir::Time);
    for (const auto& entry : entries) files.push_back(entry.fileName());
    return files;
}

void Service::PruneSupportLogs()
{
    const auto prune = [](const QString& directory, int maximumFiles, qint64 maximumBytes) {
        const auto entries = QDir(directory).entryInfoList({"*.log"}, QDir::Files, QDir::Time);
        qint64 retainedBytes = 0;
        int retainedFiles = 0;
        for (const auto& entry : entries)
        {
            retainedBytes += entry.size();
            ++retainedFiles;
            if (retainedFiles > maximumFiles || retainedBytes > maximumBytes)
                QFile::remove(entry.absoluteFilePath());
        }
    };
    prune(SupportLogDirectory, 60, 5 * 1024 * 1024);
    prune(RawLogDirectory, 20, 20 * 1024 * 1024);
}

QString Service::BuildSupportReport() const
{
    QString report;
    QTextStream out(&report);
    const auto status = Status(true);
    QString driver = "unknown";
    QString hardware = "unknown";
    if (!m_interface.isEmpty())
    {
        const QFileInfo driverLink("/sys/class/net/" + m_interface + "/device/driver");
        const QString target = driverLink.canonicalFilePath();
        if (!target.isEmpty()) driver = QFileInfo(target).fileName();
        QFile modalias("/sys/class/net/" + m_interface + "/device/modalias");
        if (modalias.open(QIODevice::ReadOnly | QIODevice::Text)) hardware = QString::fromUtf8(modalias.readLine()).trimmed();
    }
    const QString code = m_errorCode.isEmpty() && !m_error.isEmpty() ? DiagnosticCodeForError(m_error) : m_errorCode;
    const auto advice = barista::api::AdviceForDiagnostic(code.toStdString());
    QStringList missingTools;
    for (const auto& tool : status.health.missingTools) missingTools.push_back(QString::fromStdString(tool));
    out << "Barista support report\n"
        << "schema_version=1\n"
        << "generated_utc=" << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) << '\n'
        << "barista_version=" << BARISTA_VERSION_STRING << '\n'
        << "api_version=" << status.apiVersion << '\n'
        << "platform=" << QSysInfo::prettyProductName() << '\n'
        << "kernel=" << QSysInfo::kernelType() << ' ' << QSysInfo::kernelVersion() << '\n'
        << "session_id=" << (m_sessionId.isEmpty() ? "none" : m_sessionId) << '\n'
        << "run_log=" << (m_runLogName.isEmpty() ? "none" : m_runLogName) << '\n'
        << "phase=" << QString::fromLatin1(barista::api::SessionPhaseName(status.phase)) << '\n'
        << "mode=" << (status.mode ? QString::fromLatin1(barista::api::SessionModeName(*status.mode)) : QString("none")) << '\n'
        << "running=" << (status.running ? "yes" : "no") << '\n'
        << "gamepad_connected=" << (status.gamePadConnected ? "yes" : "no") << '\n'
        << "interface=" << (m_interface.isEmpty() ? "none" : m_interface) << '\n'
        << "wifi_driver=" << driver << '\n'
        << "wifi_hardware=" << hardware << '\n'
        << "network_manager=" << (status.health.networkManagerRunning ? "running" : "unavailable") << '\n'
        << "authorization_service=" << (status.health.authorizationRunning ? "running" : "unavailable") << '\n'
        << "engine_installed=" << (status.health.engineInstalled ? "yes" : "no") << '\n'
        << "wifi_helper_installed=" << (status.health.hostapdInstalled ? "yes" : "no") << '\n'
        << "virtual_controller=" << (status.capabilities.controller ? "ready" : "unavailable") << '\n'
        << "missing_tools=" << (missingTools.isEmpty() ? "none" : missingTools.join(',')) << '\n'
        << "legacy_session_present=" << (status.health.legacySessionPresent ? "yes" : "no") << '\n';
    if (!code.isEmpty())
        out << "diagnostic_code=" << code << '\n'
            << "diagnosis=" << QString::fromUtf8(advice.summary.data(), static_cast<qsizetype>(advice.summary.size())) << '\n'
            << "suggested_action=" << QString::fromUtf8(advice.action.data(), static_cast<qsizetype>(advice.action.size())) << '\n';
    if (!m_latestMediaTiming.isEmpty()) out << "latest_media_timing=" << m_latestMediaTiming << '\n';
    if (!m_latestTransportStats.isEmpty()) out << "latest_transport_stats=" << m_latestTransportStats << '\n';
    out << "\nRecent events\n";
    for (const auto& event : m_diagnosticEvents) out << event << '\n';
    out << "\nPrivacy\nThis report excludes MAC addresses, IP addresses, SSIDs, pairing codes, credentials, usernames, and raw hostapd output.\n";
    return report;
}
void Service::Authorize(std::function<QString(uint,const QString&)> operation)
{
    AuthorizeAsync([operation](uint uid,const QString& caller,Completion done) { done(operation(uid,caller)); });
}
void Service::AuthorizeAsync(std::function<void(uint,const QString&,Completion)> operation)
{
    if (!calledFromDBus()) return;
    if (m_authorizing || m_stopping) { sendErrorReply("org.barista.Error.Busy", "Another operation is pending"); return; }
    const auto request = message();
    const QString caller = request.service();
    auto bus = QDBusConnection::systemBus();
    const QDBusReply<uint> uid = bus.interface()->serviceUid(caller);
    if (!uid.isValid() || !caller.startsWith(':')) { sendErrorReply("org.barista.Error.Caller", "Cannot identify caller"); return; }
    setDelayedReply(true);
    m_authorizing = true;
    auto* check = new QProcess(this);
    auto* timeout = new QTimer(check); timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, check, &QProcess::kill);
    timeout->start(120000);
    auto complete = [this, check, bus, request, caller, expectedUid=uid.value(), operation](bool allowed) {
        check->deleteLater();
        const QDBusReply<uint> current = bus.interface()->serviceUid(caller);
        auto done = [this,bus,request](QString error) {
            m_authorizing = false;
            m_error = error;
            m_errorCode = error.isEmpty() ? QString() : DiagnosticCodeForError(error);
            if (!error.isEmpty() && (m_diagnosticEvents.isEmpty() ||
                !m_diagnosticEvents.constLast().contains(QString(" %1:").arg(m_errorCode))))
                RecordDiagnostic(m_errorCode);
            bus.send(error.isEmpty() ? request.createReply() : request.createErrorReply("org.barista.Error.Operation",error));
        };
        if (!allowed || !current.isValid() || current.value() != expectedUid) done("Authorization denied or caller disconnected");
        else operation(expectedUid,caller,done);
    };
    connect(check, qOverload<int,QProcess::ExitStatus>(&QProcess::finished), this,
        [complete](int code,QProcess::ExitStatus status) { complete(status == QProcess::NormalExit && code == 0); });
    connect(check, &QProcess::errorOccurred, this, [complete](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) complete(false);
    });
    QProcessEnvironment authEnv;
    authEnv.insert("PATH","/usr/sbin:/usr/bin:/sbin:/bin");
    check->setProcessEnvironment(authEnv);
    check->start(BARISTA_PKCHECK, {"--action-id","org.barista.manage-session","--system-bus-name",caller,"--allow-user-interaction"});
}
void Service::StartSession(const QString& interface, const QString& mode)
{
    const auto parsedMode = barista::api::ParseSessionMode(mode.toStdString());
    AuthorizeAsync([this,interface,parsedMode](uint uid,const QString& caller,Completion done) {
        if (!barista::api::ValidInterfaceName(interface.toStdString()) || !QFileInfo::exists("/sys/class/net/" + interface + "/phy80211") ||
            !parsedMode) { done("Choose an existing wireless adapter and supported mode"); return; }
        if (m_worker.state() != QProcess::NotRunning) { done("Stop the current session before starting another one"); return; }
        m_error.clear(); m_errorCode.clear(); m_mode = *parsedMode; m_interface = interface;
        StartSupportRun(ModeName(*parsedMode));
        Prepare(*parsedMode == barista::api::SessionMode::Controller,caller,[this,interface,mode=*parsedMode,uid,caller,done](QString error) {
            if (!error.isEmpty()) { done(error); CloseSupportRun(); return; }
            done(Start(interface,mode,{},uid,caller));
        });
    });
}
void Service::Pair(const QString& interface, const QString& code, const QString& mode)
{
    if (!barista::api::ParsePairCode(code.toStdString())) { sendErrorReply("org.barista.Error.Invalid", "Pairing code must be four digits 0–3"); return; }
    const auto parsedMode = barista::api::ParseSessionMode(mode.toStdString());
    AuthorizeAsync([this,interface,code,parsedMode](uint uid,const QString& caller,Completion done) {
        if (!barista::api::ValidInterfaceName(interface.toStdString()) || !QFileInfo::exists("/sys/class/net/" + interface + "/phy80211") ||
            !parsedMode) { done("Choose an existing wireless adapter and supported mode"); return; }
        if (m_worker.state() != QProcess::NotRunning) { done("Stop the current session before starting another one"); return; }
        m_error.clear(); m_errorCode.clear(); m_mode = *parsedMode; m_interface = interface;
        StartSupportRun(ModeName(*parsedMode));
        Prepare(*parsedMode == barista::api::SessionMode::Controller,caller,[this,interface,code,mode=*parsedMode,uid,caller,done](QString error) {
            if (!error.isEmpty()) { done(error); CloseSupportRun(); return; }
            done(Start(interface,mode,code,uid,caller));
        });
    });
}
void Service::PrepareSystem()
{
    AuthorizeAsync([this](uint,const QString& caller,Completion done) {
        if (m_worker.state() != QProcess::NotRunning) { done("Stop the current session before preparing system services"); return; }
        m_error.clear(); m_errorCode.clear(); m_interface.clear(); m_mode.reset();
        StartSupportRun("maintenance");
        Prepare(true,caller,[this,done](QString error) { done(error); CloseSupportRun(); });
    });
}
void Service::RunSetup(const QString& program, const QStringList& args, Completion done)
{
    // Only fixed internal calls below. No caller-supplied command, unit or module.
    if (!TrustedSystemHelper(program)) { done("Required system helper is missing or not securely installed: " + program); return; }
    auto* process = new QProcess(this);
    auto* timer = new QTimer(process); timer->setSingleShot(true);
    auto finished = std::make_shared<bool>(false);
    auto complete = [process,timer,finished,done](QString error) {
        if (std::exchange(*finished,true)) return;
        timer->stop(); process->deleteLater(); done(error);
    };
    connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,
        [complete](int code,QProcess::ExitStatus status) {
            complete(status == QProcess::NormalExit && code == 0 ? QString() : "System preparation failed. Check the service journal for details.");
        });
    connect(process,&QProcess::errorOccurred,this,[complete](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) complete("Could not launch the installed system helper");
    });
    connect(timer,&QTimer::timeout,process,[process] { process->kill(); });
    QProcessEnvironment env; env.insert("PATH","/usr/sbin:/usr/bin:/sbin:/bin"); env.insert("LANG","C.UTF-8");
    process->setProcessEnvironment(env); process->setProcessChannelMode(QProcess::ForwardedChannels);
    timer->start(10000); process->start(program,args);
}
void Service::Prepare(bool controller, const QString& caller, Completion done)
{
    if (m_worker.state() != QProcess::NotRunning) { done("Stop the current session before preparing system services"); return; }
    if (QFileInfo::exists("/tmp/drcd.sock")) { done("Stop the legacy drcd/capture session before preparing system services"); return; }
    if (!BusServiceRunning(caller)) { done("Caller disconnected"); return; }
    RecordDiagnostic("SYSTEM_PREPARATION_STARTED");
    auto finish = [this,done](QString error) {
        if (error.isEmpty()) RecordDiagnostic("SYSTEM_PREPARATION_SUCCEEDED");
        done(error);
    };
    auto loadController = [this,controller,caller,finish](QString error) {
        if (!error.isEmpty()) { finish(error); return; }
        if (!BusServiceRunning(caller)) { finish("Caller disconnected"); return; }
        if (!BusServiceRunning("org.freedesktop.NetworkManager")) { finish("NetworkManager did not become available"); return; }
        if (controller && !QFileInfo::exists("/dev/uinput")) {
            RunSetup(BARISTA_MODPROBE,{"uinput"},[caller,finish](QString result) {
                if (!BusServiceRunning(caller)) finish("Caller disconnected");
                else if (!result.isEmpty()) finish(result);
                else finish(QFileInfo::exists("/dev/uinput") ? QString() : "This kernel did not provide /dev/uinput. Controller only mode is unavailable.");
            });
        } else finish({});
    };
    if (!BusServiceRunning("org.freedesktop.NetworkManager"))
        RunSetup(BARISTA_SYSTEMCTL,{"start","NetworkManager.service"},loadController);
    else loadController({});
}
void Service::StopSession()
{
    Authorize([this](uint,const QString& caller) -> QString {
        if (!m_owner.isEmpty() && caller != m_owner) return "Session belongs to another client";
        StopWorker(); return {};
    });
}
QString Service::Start(const QString& interface, barista::api::SessionMode mode, const QString& code, uint uid, const QString& caller)
{
    if (m_worker.state() != QProcess::NotRunning) {
        RecordDiagnostic("SESSION_BUSY"); CloseSupportRun(); return "Stop the current session before changing mode or pairing";
    }
    if (!barista::api::ValidInterfaceName(interface.toStdString()) || !QFileInfo::exists("/sys/class/net/" + interface + "/phy80211"))
    { RecordDiagnostic("INVALID_REQUEST"); CloseSupportRun(); return "Choose an existing wireless interface"; }
    if (!TrustedExecutable(BARISTA_WORKER) || !TrustedExecutable(BARISTA_HOSTAPD)) {
        RecordDiagnostic("ENGINE_START_FAILED"); CloseSupportRun();
        return "Install root-owned Barista engine and hostapd binaries first; writable development binaries cannot run privileged";
    }
    if (QFileInfo::exists("/tmp/drcd.sock")) {
        RecordDiagnostic("SESSION_BUSY"); CloseSupportRun(); return "Stop the legacy drcd/capture session first (existing /tmp/drcd.sock)";
    }
    m_error.clear(); m_errorCode.clear(); m_phase = "idle"; m_connected = false; m_batteryAvailable = false; m_battery = 0;
    m_mode = mode; m_interface = interface; m_uid = uid;
    m_endpoint = QString("/run/barista/media-%1.sock").arg(uid);
    // Runtime directory is root-owned; only remove our exact previous socket,
    // never a regular file, symlink or arbitrary caller-supplied path.
    struct stat old{};
    if (lstat(m_endpoint.toLocal8Bit().constData(), &old) == 0) {
        if (!S_ISSOCK(old.st_mode)) {
            RecordDiagnostic("ENGINE_START_FAILED"); CloseSupportRun();
            return "Media endpoint exists and is not a socket";
        }
        if (unlink(m_endpoint.toLocal8Bit().constData())) {
            RecordDiagnostic("ENGINE_START_FAILED"); CloseSupportRun();
            return "Cannot remove stale media endpoint";
        }
    }
    QString idleError;
    if (!barista::WriteIdleScreen("/run/barista/idle.i420", idleError)) {
        RecordDiagnostic("ENGINE_START_FAILED", "media"); CloseSupportRun(); return idleError;
    }
    if (mode == barista::api::SessionMode::Controller) {
        std::string error;
        if (!m_controller.Start(error)) {
            m_phase = "idle"; m_errorCode = "CONTROLLER_UNAVAILABLE";
            RecordDiagnostic(m_errorCode, "controller"); CloseSupportRun();
            return error.empty() ? "Cannot create virtual controller" : QString::fromStdString(error);
        }
        m_input = std::make_unique<barista::api::AppHook>(false);
        if (!m_input->start(m_endpoint.toStdString(), error)) {
            m_controller.Stop(); m_input.reset(); m_errorCode = "ENGINE_START_FAILED";
            RecordDiagnostic(m_errorCode, "media"); CloseSupportRun();
            return QString::fromStdString(error);
        }
    }
    QProcessEnvironment env;
    env.insert("PATH","/usr/sbin:/usr/bin:/sbin:/bin");
    env.insert("LANG","C.UTF-8");
    env.insert("DRCD_HOSTAPD_BIN",BARISTA_HOSTAPD);
    env.insert("DRCD_CREDENTIALS_FILE","/var/lib/drcd/credentials.conf");
    env.insert("DRCD_LOG_FILE",QString::fromLatin1(RawLogDirectory) + "/private-" + m_runLogName);
    // Prefer the known-good non-DFS Wii U pairing channel before the fallback sweep.
    env.insert("DRCD_AP_CHANNEL","149");
    env.insert("BARISTA_MUG_SOCKET",m_endpoint);
    env.insert("BARISTA_IDLE_I420","/run/barista/idle.i420");
    env.insert("BARISTA_CLIENT_UID",QString::number(mode == barista::api::SessionMode::Controller ? 0 : uid));
    env.insert("BARISTA_SESSION_ID",m_sessionId);
    env.insert("DRCD_LOG_STDERR","1");
    QStringList args{"--socket",ControlSocket,"--interface",interface};
    if (code.isEmpty()) args << "--np";
    else args << "--pair-code" << code << "--pair";
    m_owner = caller; m_stopping = false;
    findChild<QDBusServiceWatcher*>()->addWatchedService(caller);
    m_worker.setProcessEnvironment(env);
    m_worker.setWorkingDirectory("/var/lib/barista");
    m_workerOutput.clear();
    m_phase = "starting";
    m_worker.start(BARISTA_WORKER,args);
    return {};
}
void Service::StopWorker()
{
    m_controller.Stop(); m_input.reset();
    if (m_worker.state() != QProcess::NotRunning) {
        if (!m_stopping) RecordDiagnostic("SESSION_STOPPED");
        m_stopping = true; m_phase = "stopping"; m_worker.terminate();
    }
    else { m_owner.clear(); m_connected = false; m_batteryAvailable = false; m_battery = 0; m_phase = "idle"; }
}
void Service::Poll()
{
    if (m_worker.state() != QProcess::Running || m_statusSocket.state() != QLocalSocket::UnconnectedState) return;
    m_response.clear();
    m_statusSocket.connectToServer(ControlSocket);
    m_statusTimeout.start(1200);
}
void Service::ParseStatus()
{
    m_statusTimeout.stop();
    if (m_worker.state() != QProcess::Running || m_stopping || m_response.size() > 16384 || !m_response.startsWith("OK ")) return;
    for (const auto& line : m_response.split('\n')) {
        if (line.startsWith("phase=")) m_phase = QString::fromUtf8(line.mid(6));
        if (line.startsWith("connected=")) m_connected = line.mid(10) == "1";
        if (line.startsWith("battery_available=")) m_batteryAvailable = line.mid(18) == "1";
        if (line.startsWith("battery=")) m_battery = BatteryPercent(line.mid(8).toInt());
        // Deliberately do not expose PINs, credentials, or arbitrary engine logs.
    }
}
