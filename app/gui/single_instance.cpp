#include "single_instance.h"
#include <QLocalSocket>
#include <QTimer>
#include <QDebug>

SingleInstance::SingleInstance(const QString& path) : m_path(path), m_lock(path + ".lock")
{
    m_lock.setStaleLockTime(0);
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server,&QLocalServer::newConnection,this,[this] {
        while (auto* socket = m_server.nextPendingConnection()) {
            connect(socket,&QLocalSocket::disconnected,socket,&QObject::deleteLater);
            auto read = [this,socket] {
                if (socket->bytesAvailable() > 16) { socket->abort(); return; }
                if (!socket->canReadLine()) return;
                if (socket->readLine() == "show\n") emit ShowRequested();
                socket->disconnectFromServer();
            };
            connect(socket,&QLocalSocket::readyRead,this,read);
            QTimer::singleShot(1000,socket,[socket] { socket->abort(); });
            read();
        }
    });
}
SingleInstance::Result SingleInstance::Start()
{
    if (!m_lock.tryLock()) {
        qWarning() << "Barista UI lock failed" << m_lock.error();
        QLocalSocket socket;
        socket.connectToServer(m_path);
        if (!socket.waitForConnected(1000)) { qWarning() << "Barista UI forwarding failed" << socket.errorString(); return Result::Error; }
        if (socket.write("show\n") != 5 || !socket.waitForBytesWritten(1000)) return Result::Error;
        return Result::Forwarded;
    }
    // The lock proves no other Barista UI owns this endpoint. Remove only this
    // user's stale GUI listener, never the privileged engine/media sockets.
    QLocalServer::removeServer(m_path);
    if (!m_server.listen(m_path)) { qWarning() << "Barista UI listener failed" << m_server.errorString(); return Result::Error; }
    return Result::Primary;
}
