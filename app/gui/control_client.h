#pragma once
#include "api/types.h"
#include <QObject>
#include <QStringList>
#include <QVariantList>

// Transport seam: the widget layer has no D-Bus, polkit, uinput or Unix headers.
class ControlClient : public QObject {
    Q_OBJECT
public:
    explicit ControlClient(QObject* parent = nullptr) : QObject(parent) {}
    void Refresh();
    void Start(const barista::api::StartSessionRequest& request);
    void Pair(const barista::api::PairRequest& request);
    void Stop();
    void Prepare();
    void Retry();
    void RefreshGamePads();
    void RefreshDiagnostics();
    void RenameGamePad(const barista::api::RenameGamePadRequest& request);
    void RemoveGamePad(const barista::api::RemoveGamePadRequest& request);
signals:
    void Status(const barista::api::SessionStatus& status);
    void GamePads(const std::vector<barista::api::GamePad>& gamePads);
    void Error(const QString& message);
    void Pending(bool pending);
    void Stopped(bool success);
    void Diagnostics(const QString& report, const QString& directory, const QStringList& files, const QString& sessionId);
private:
    void Call(const QString& method, const QVariantList& arguments = {});
    bool m_pollPending = false, m_operationPending = false;
    bool m_stopAfterOperation = false;
    qint64 m_nextRetry = 0;
};
