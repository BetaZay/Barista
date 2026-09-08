#include "service.h"
#include "../branding/idle_screen.h"
#include "api/controller.h"
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
constexpr int GamePadBatteryFull = 176;

QString ModeName(barista::api::SessionMode mode)
{
    return QString::fromLatin1(barista::api::SessionModeName(mode));
}

int BatteryPercent(int raw)
{
    return qBound(0, (raw * 100 + GamePadBatteryFull / 2) / GamePadBatteryFull, 100);
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
    m_worker.setProcessChannelMode(QProcess::ForwardedChannels);
    connect(&m_worker, &QProcess::started, this, [this] {
        const QDBusReply<bool> alive = QDBusConnection::systemBus().interface()->isServiceRegistered(m_owner);
        if (m_stopping || !alive.isValid() || !alive.value()) StopWorker();
    });
    connect(&m_worker, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        m_error = m_worker.errorString();
        if (error == QProcess::FailedToStart) {
            m_controller.Stop(); m_input.reset(); m_owner.clear(); m_phase = "idle"; m_connected = false;
        }
    });
    connect(&m_worker, qOverload<int,QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, auto) {
        m_controller.Stop(); m_input.reset(); m_connected = false; m_phase = "idle";
        if (!m_stopping) m_error = QString("Radio engine exited (%1); inspect journalctl -u barista").arg(code);
        m_stopping = false; m_owner.clear(); m_endpoint.clear();
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
        if (!m_controller.Submit(state)) { m_error = "Virtual controller write failed; session stopped"; StopWorker(); }
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
        status.error = barista::api::Error{
            .code = barista::api::ErrorCode::Failed,
            .message = m_error.toStdString(),
        };
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
    return {{"apiVersion",status.apiVersion}, {"platform",QString::fromStdString(status.platform)},
        {"running",status.running}, {"phase",QString::fromLatin1(barista::api::SessionPhaseName(status.phase))},
        {"connected",status.gamePadConnected}, {"mode",status.mode ? ModeName(*status.mode) : QString()},
        {"interface",QString::fromStdString(status.interfaceName)},
        {"batteryAvailable",status.batteryPercent.has_value()}, {"battery",status.batteryPercent.value_or(0)},
        {"ownedByCaller",status.ownedByCaller}, {"busy",status.busy}, {"error",error},
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
        Prepare(*parsedMode == barista::api::SessionMode::Controller,caller,[this,interface,mode=*parsedMode,uid,caller,done](QString error) {
            done(error.isEmpty() ? Start(interface,mode,{},uid,caller) : error);
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
        Prepare(*parsedMode == barista::api::SessionMode::Controller,caller,[this,interface,code,mode=*parsedMode,uid,caller,done](QString error) {
            done(error.isEmpty() ? Start(interface,mode,code,uid,caller) : error);
        });
    });
}
void Service::PrepareSystem()
{
    AuthorizeAsync([this](uint,const QString& caller,Completion done) { Prepare(true,caller,done); });
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
    auto loadController = [this,controller,caller,done](QString error) {
        if (!error.isEmpty()) { done(error); return; }
        if (!BusServiceRunning(caller)) { done("Caller disconnected"); return; }
        if (!BusServiceRunning("org.freedesktop.NetworkManager")) { done("NetworkManager did not become available"); return; }
        if (controller && !QFileInfo::exists("/dev/uinput")) {
            RunSetup(BARISTA_MODPROBE,{"uinput"},[caller,done](QString result) {
                if (!BusServiceRunning(caller)) done("Caller disconnected");
                else if (!result.isEmpty()) done(result);
                else done(QFileInfo::exists("/dev/uinput") ? QString() : "This kernel did not provide /dev/uinput. Controller only mode is unavailable.");
            });
        } else done({});
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
    if (m_worker.state() != QProcess::NotRunning) return "Stop the current session before changing mode or pairing";
    if (!barista::api::ValidInterfaceName(interface.toStdString()) || !QFileInfo::exists("/sys/class/net/" + interface + "/phy80211"))
        return "Choose an existing wireless interface";
    if (!TrustedExecutable(BARISTA_WORKER) || !TrustedExecutable(BARISTA_HOSTAPD))
        return "Install root-owned Barista engine and hostapd binaries first; writable development binaries cannot run privileged";
    if (QFileInfo::exists("/tmp/drcd.sock")) return "Stop the legacy drcd/capture session first (existing /tmp/drcd.sock)";
    m_error.clear(); m_phase = "idle"; m_connected = false; m_batteryAvailable = false; m_battery = 0;
    m_mode = mode; m_interface = interface; m_uid = uid;
    m_endpoint = QString("/run/barista/media-%1.sock").arg(uid);
    // Runtime directory is root-owned; only remove our exact previous socket,
    // never a regular file, symlink or arbitrary caller-supplied path.
    struct stat old{};
    if (lstat(m_endpoint.toLocal8Bit().constData(), &old) == 0) {
        if (!S_ISSOCK(old.st_mode)) return "Media endpoint exists and is not a socket";
        if (unlink(m_endpoint.toLocal8Bit().constData())) return "Cannot remove stale media endpoint";
    }
    QString idleError;
    if (!barista::WriteIdleScreen("/run/barista/idle.i420", idleError)) return idleError;
    if (mode == barista::api::SessionMode::Controller) {
        std::string error;
        if (!m_controller.Start(error)) { m_phase = "idle"; return error.empty() ? "Cannot create virtual controller" : QString::fromStdString(error); }
        m_input = std::make_unique<barista::api::AppHook>(false);
        if (!m_input->start(m_endpoint.toStdString(), error)) { m_controller.Stop(); m_input.reset(); return QString::fromStdString(error); }
    }
    QProcessEnvironment env;
    env.insert("PATH","/usr/sbin:/usr/bin:/sbin:/bin");
    env.insert("LANG","C.UTF-8");
    env.insert("DRCD_HOSTAPD_BIN",BARISTA_HOSTAPD);
    env.insert("DRCD_CREDENTIALS_FILE","/var/lib/drcd/credentials.conf");
    env.insert("DRCD_LOG_FILE","/var/log/barista/engine.log");
    // Prefer the known-good non-DFS Wii U pairing channel before the fallback sweep.
    env.insert("DRCD_AP_CHANNEL","149");
    env.insert("BARISTA_MUG_SOCKET",m_endpoint);
    env.insert("BARISTA_IDLE_I420","/run/barista/idle.i420");
    env.insert("BARISTA_CLIENT_UID",QString::number(mode == barista::api::SessionMode::Controller ? 0 : uid));
    env.insert("DRCD_LOG_STDERR","1");
    QStringList args{"--socket",ControlSocket,"--interface",interface};
    if (code.isEmpty()) args << "--np";
    else args << "--pair-code" << code << "--pair";
    m_owner = caller; m_stopping = false;
    findChild<QDBusServiceWatcher*>()->addWatchedService(caller);
    m_worker.setProcessEnvironment(env);
    m_worker.setWorkingDirectory("/var/lib/barista");
    m_phase = "starting";
    m_worker.start(BARISTA_WORKER,args);
    return {};
}
void Service::StopWorker()
{
    m_controller.Stop(); m_input.reset();
    if (m_worker.state() != QProcess::NotRunning) { m_stopping = true; m_phase = "stopping"; m_worker.terminate(); }
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
