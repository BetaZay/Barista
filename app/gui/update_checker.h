#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QString>
#include <QSysInfo>
#include <QUrl>

class UpdateChecker : public QObject
{
    Q_OBJECT
public:
    // Explicit dependencies make network/timing behavior testable without
    // changing real system configuration or contacting the production feed.
    struct Environment
    {
        QString channelPath = "/etc/barista/update-channel";
        QString osReleasePath = "/etc/os-release";
        QString architecture = QSysInfo::currentCpuArchitecture();
        QUrl endpoint = QUrl("https://betazay.github.io/Barista/updates/v1.json");
        int timeoutMs = 10000;
    };
    explicit UpdateChecker(QObject* parent = nullptr);
    UpdateChecker(const Environment& environment, QObject* parent);
    void Check(bool manual = false);
    static QString Target(const QByteArray& osRelease, const QString& architecture);
    static bool Newer(const QString& candidate, const QString& installed);
    static QString Available(const QByteArray& json, const QString& channel,
                             const QString& target, const QString& installed);
    static QString Channel();
signals:
    void Status(const QString& message);
    void UpdateAvailable(const QString& version);
private:
    QNetworkAccessManager m_network;
    Environment m_environment;
    bool m_checking = false;
};
