#include "control_client.h"
#include <QDateTime>
#include <QTimer>
#ifdef BARISTA_LINUX_CONTROL
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif
void ControlClient::Refresh() { if (!m_pollPending && QDateTime::currentMSecsSinceEpoch() >= m_nextRetry) Call("GetStatus"); }
void ControlClient::Retry() { m_nextRetry = 0; Refresh(); }
void ControlClient::Prepare() { Call("PrepareSystem"); }
void ControlClient::Start(const QString& interface, const QString& mode) { Call("StartSession",{interface,mode}); }
void ControlClient::Pair(const QString& interface, const QString& code, const QString& mode) { Call("Pair",{interface,code,mode}); }
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
                emit Status({{"available",false},{"error","Automatic service startup failed. Check the Barista installation and system D-Bus. " + reply.error().message()}});
            } else { m_nextRetry = 0; auto status = reply.value(); status.insert("available",true); emit Status(status); }
        } else {
            m_operationPending = false; emit Pending(false);
            QDBusPendingReply<> reply = *completed;
            const bool success = !reply.isError();
            if (!success) emit Error(reply.error().message());
            if (method == "StopSession") emit Stopped(success);
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
    emit Status({{"available",false},{"error","No radio/service backend is implemented on this platform yet."}});
#endif
}
