#pragma once
#include <QObject>
#include <QLocalServer>
#include <QLockFile>

// One user-owned UI keeps the D-Bus session owner alive while in the tray.
class SingleInstance : public QObject {
    Q_OBJECT
public:
    enum class Result { Primary, Forwarded, Error };
    explicit SingleInstance(const QString& path);
    Result Start();
signals:
    void ShowRequested();
private:
    QString m_path;
    QLockFile m_lock;
    QLocalServer m_server;
};
