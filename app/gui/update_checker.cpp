#include "update_checker.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSettings>
#include <QSysInfo>
#include <QTimer>
#include <QVersionNumber>
#include <memory>

namespace {
constexpr qint64 MaxResponse = 256 * 1024;
bool ValidVersion(const QString& value)
{
    static const QRegularExpression pattern("^(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})$");
    return pattern.match(value).hasMatch();
}
QByteArray Read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.read(4096) : QByteArray();
}
}

UpdateChecker::UpdateChecker(QObject* parent) : UpdateChecker(Environment{},parent) {}
UpdateChecker::UpdateChecker(const Environment& environment, QObject* parent)
    : QObject(parent), m_network(this), m_environment(environment) {}

QString UpdateChecker::Channel()
{
    const QString channel = QString::fromUtf8(Read("/etc/barista/update-channel")).trimmed();
    return channel == "stable" || channel == "preview" ? channel : QString();
}

QString UpdateChecker::Target(const QByteArray& osRelease, const QString& architecture)
{
    if (architecture != "x86_64") return {};
    QMap<QString,QString> values;
    for (const auto& line : osRelease.split('\n')) {
        const int equal = line.indexOf('=');
        if (equal <= 0) continue;
        QString value = QString::fromUtf8(line.mid(equal + 1)).trimmed();
        if (value.size() >= 2 && ((value.startsWith('"') && value.endsWith('"')) ||
                                 (value.startsWith('\'') && value.endsWith('\''))))
            value = value.mid(1,value.size() - 2);
        values.insert(QString::fromUtf8(line.left(equal)),value);
    }
    if (values.value("ID") == "ubuntu" && values.value("VERSION_ID") == "24.04") return "ubuntu-24.04-x86_64";
    if (values.value("ID") == "fedora" && values.value("VERSION_ID") == "44") return "fedora-44-x86_64";
    if (values.value("ID") == "arch") return "arch-x86_64";
    return {};
}

bool UpdateChecker::Newer(const QString& candidate, const QString& installed)
{
    return ValidVersion(candidate) && ValidVersion(installed) &&
        QVersionNumber::compare(QVersionNumber::fromString(candidate),QVersionNumber::fromString(installed)) > 0;
}

QString UpdateChecker::Available(const QByteArray& json, const QString& channel,
                                 const QString& target, const QString& installed)
{
    if (json.size() > MaxResponse || target.isEmpty() || (channel != "stable" && channel != "preview")) return {};
    const auto document = QJsonDocument::fromJson(json);
    const auto root = document.object();
    if (root.value("schema").toInt() != 1) return {};
    const auto release = root.value("channels").toObject().value(channel).toObject();
    const QString version = release.value("version").toString();
    return release.value("targets").toArray().contains(target) && Newer(version,installed) ? version : QString();
}

void UpdateChecker::Check(bool manual)
{
    if (m_checking) return;
    QSettings settings;
    if (!manual && !settings.value("updates/enabled",true).toBool()) return;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 previous = settings.value("updates/lastAttempt",0).toLongLong();
    if (!manual && now >= previous && now - previous < 86400) return;
    const QString installed = QCoreApplication::applicationVersion();
    const QString channel = QString::fromUtf8(Read(m_environment.channelPath)).trimmed();
    const QString target = Target(Read(m_environment.osReleasePath),m_environment.architecture);
    if (!ValidVersion(installed) || installed.endsWith(".0") || (channel != "stable" && channel != "preview") || target.isEmpty()) {
        emit Status("Update repositories are not configured for this installation. See update setup instructions.");
        return;
    }
    settings.setValue("updates/lastAttempt",now);
    m_checking = true;
    if (manual) emit Status("Checking for updates…");
    QNetworkRequest request(m_environment.endpoint);
    request.setTransferTimeout(m_environment.timeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::SameOriginRedirectPolicy);
    auto* reply = m_network.get(request);
    auto body = std::make_shared<QByteArray>();
    connect(reply,&QNetworkReply::readyRead,this,[reply,body] {
        if (body->size() > MaxResponse) { reply->abort(); return; }
        body->append(reply->read(MaxResponse + 1 - body->size()));
        if (body->size() > MaxResponse) reply->abort();
    });
    QTimer::singleShot(m_environment.timeoutMs,reply,[reply] { if (!reply->isFinished()) reply->abort(); });
    connect(reply,&QNetworkReply::finished,this,[this,reply,body,manual,channel,target,installed] {
        m_checking = false;
        const auto document = QJsonDocument::fromJson(*body);
        const auto release = document.object().value("channels").toObject().value(channel).toObject();
        const bool valid = reply->error() == QNetworkReply::NoError && body->size() <= MaxResponse &&
            document.object().value("schema").toInt() == 1 && ValidVersion(release.value("version").toString()) &&
            release.value("targets").toArray().contains(target);
        if (!valid) {
            if (manual) emit Status("Could not check for updates. Try again later.");
        } else {
            const QString version = Available(*body,channel,target,installed);
            emit Status(version.isEmpty() ? "You’re up to date for your subscribed channel." :
                "Barista " + version + " is available through your system updater.");
            if (!version.isEmpty()) emit UpdateAvailable(version);
        }
        reply->deleteLater();
    });
}
