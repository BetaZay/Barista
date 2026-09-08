#pragma once
#include "api/types.h"
#include <QObject>
#include <QVariantMap>

// Transport seam: the widget layer has no D-Bus, polkit, uinput or Unix headers.
class ControlClient : public QObject {
    Q_OBJECT
public:
    explicit ControlClient(QObject* parent = nullptr) : QObject(parent) {}
    void Refresh();
    void Start(const QString& interface, barista::api::SessionMode mode);
    void Pair(const QString& interface, const QString& code, barista::api::SessionMode mode);
    void Stop();
    void Prepare();
    void Retry();
signals:
    void Status(const QVariantMap& status);
    void Error(const QString& message);
    void Pending(bool pending);
    void Stopped(bool success);
private:
    void Call(const QString& method, const QVariantList& arguments = {});
    bool m_pollPending = false, m_operationPending = false;
    bool m_stopAfterOperation = false;
    qint64 m_nextRetry = 0;
};
