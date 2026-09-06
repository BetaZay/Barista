#pragma once
#include <QObject>
#include <QVariantMap>

// Transport seam: the widget layer has no D-Bus, polkit, uinput or Unix headers.
class ControlClient : public QObject {
    Q_OBJECT
public:
    explicit ControlClient(QObject* parent = nullptr) : QObject(parent) {}
    void Refresh();
    void Start(const QString& interface, const QString& mode);
    void Pair(const QString& interface, const QString& code, const QString& mode);
    void Stop();
    void Prepare();
    void Retry();
signals:
    void Status(const QVariantMap& status);
    void Error(const QString& message);
    void Pending(bool pending);
private:
    void Call(const QString& method, const QVariantList& arguments = {});
    bool m_pollPending = false, m_operationPending = false;
    qint64 m_nextRetry = 0;
};
