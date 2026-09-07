#pragma once
#include <QObject>
#include <QDBusContext>
#include <QProcess>
#include <QLocalSocket>
#include <QTimer>
#include <QVariantMap>
#include <functional>
#include <memory>
#include "drc_ipc/app_hook.h"
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
    QVariantList SavedGamePads();
    void RemoveGamePad(const QString& mac);
    void RenameGamePad(const QString& mac, const QString& name);
    void StartSession(const QString& interface, const QString& mode);
    void Pair(const QString& interface, const QString& code, const QString& mode);
    void StopSession();
    void PrepareSystem();
private:
    using Completion = std::function<void(QString)>;
    void AuthorizeAsync(std::function<void(uint, const QString&, Completion)> operation);
    void Authorize(std::function<QString(uint, const QString&)> operation);
    void Prepare(bool controller, const QString& caller, Completion done);
    void RunSetup(const QString& program, const QStringList& args, Completion done);
    QString Start(const QString& interface, const QString& mode, const QString& code, uint uid, const QString& caller);
    void Poll();
    void ParseStatus();
    QProcess m_worker;
    QLocalSocket m_statusSocket;
    QTimer m_poll, m_inputTimer, m_statusTimeout;
    QByteArray m_response;
    QString m_owner, m_mode, m_interface, m_endpoint, m_error, m_phase = "idle";
    uint m_uid = 0;
    bool m_authorizing = false, m_connected = false, m_stopping = false, m_batteryAvailable = false;
    int m_battery = 0;
    std::unique_ptr<drc_ipc::AppHook> m_input;
    barista::UinputOutput m_controller;
};
