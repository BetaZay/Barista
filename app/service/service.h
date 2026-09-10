#pragma once
#include <QObject>
#include <QDBusContext>
#include <QProcess>
#include <QLocalSocket>
#include <QTimer>
#include <QVariantMap>
#include <QFile>
#include <QStringList>
#include <functional>
#include <memory>
#include <optional>
#include "api/types.h"
#include "api/app_hook.h"
#include "uinput_output.h"

class Service : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.barista.Service1")
public:
    explicit Service(QObject* parent = nullptr);
    ~Service() override;
    void StopWorker();
public slots:
    QVariantMap GetStatus();
    QVariantMap GetDiagnostics();
    QVariantList SavedGamePads();
    void RemoveGamePad(const QString& mac);
    void RenameGamePad(const QString& mac, const QString& name);
    void StartSession(const QString& interface, const QString& mode);
    void Pair(const QString& interface, const QString& code, const QString& mode);
    void StopSession();
    void PrepareSystem();
private:
    using Completion = std::function<void(QString)>;
    std::vector<barista::api::GamePad> GamePads() const;
    void RenameGamePadRecord(const barista::api::RenameGamePadRequest& request);
    void RemoveGamePadRecord(const barista::api::RemoveGamePadRequest& request);
    void AuthorizeAsync(std::function<void(uint, const QString&, Completion)> operation);
    void Authorize(std::function<QString(uint, const QString&)> operation);
    void Prepare(bool controller, const QString& caller, Completion done);
    void RunSetup(const QString& program, const QStringList& args, Completion done);
    QString Start(const QString& interface, barista::api::SessionMode mode, const QString& code, uint uid, const QString& caller);
    barista::api::SessionStatus Status(bool ownedByCaller) const;
    void Poll();
    void ParseStatus();
    void ProcessWorkerOutput();
    void StartSupportRun(const QString& mode);
    void RecordDiagnostic(const QString& code, const QString& component = "service", const QString& detail = {});
    void CloseSupportRun();
    QString BuildSupportReport() const;
    QStringList SupportLogFiles() const;
    void PruneSupportLogs();
    QProcess m_worker;
    QLocalSocket m_statusSocket;
    QTimer m_poll, m_inputTimer, m_statusTimeout;
    QByteArray m_response;
    QString m_owner, m_interface, m_endpoint, m_error, m_phase = "idle";
    QString m_errorCode, m_sessionId, m_runLogName;
    QString m_latestMediaTiming, m_latestTransportStats;
    QStringList m_diagnosticEvents;
    QByteArray m_workerOutput;
    QFile m_runLog, m_pairingLog;
    int m_pairingCycle = 0;
    qint64 m_lastMediaTimingLog = 0, m_lastTransportStatsLog = 0;
    std::optional<barista::api::SessionMode> m_mode;
    uint m_uid = 0;
    bool m_authorizing = false, m_connected = false, m_stopping = false, m_batteryAvailable = false;
    int m_battery = 0;
    std::unique_ptr<barista::api::AppHook> m_input;
    barista::UinputOutput m_controller;
};
