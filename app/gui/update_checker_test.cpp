#include "update_checker.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <stdexcept>

void Check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

template<class Predicate> void Wait(Predicate complete)
{
    QElapsedTimer timer;
    timer.start();
    while (!complete() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    Check(complete(),"Timed out waiting for update request");
}

void NetworkTests()
{
    QTemporaryDir directory;
    Check(directory.isValid(),"Temporary update-test directory");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
    QCoreApplication::setOrganizationName("BaristaTests");
    QCoreApplication::setApplicationName("Updates");
    QCoreApplication::setApplicationVersion("0.1.10");
    UpdateChecker::Environment environment;
    environment.channelPath = directory.filePath("channel");
    environment.osReleasePath = directory.filePath("os-release");
    environment.architecture = "x86_64";
    environment.timeoutMs = 500;
    for (const auto& entry : {qMakePair(environment.channelPath,QByteArray("stable\n")),
                              qMakePair(environment.osReleasePath,QByteArray("ID=arch\n"))}) {
        QFile file(entry.first);
        Check(file.open(QIODevice::WriteOnly),"Open update-test configuration");
        file.write(entry.second);
    }
    QTcpServer server;
    Check(server.listen(QHostAddress::LocalHost),"Listen for fake update requests");
    environment.endpoint = QUrl(QString("http://127.0.0.1:%1/updates").arg(server.serverPort()));
    QByteArray response = R"({"schema":1,"channels":{"stable":{"version":"0.1.100","targets":["arch-x86_64"]}}})";
    int requests = 0;
    bool stall = false;
    QObject::connect(&server,&QTcpServer::newConnection,&server,[&] {
        auto* socket = server.nextPendingConnection();
        QObject::connect(socket,&QTcpSocket::readyRead,socket,[&,socket] {
            socket->readAll();
            ++requests;
            if (stall) return;
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response);
            socket->disconnectFromHost();
        });
        QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
    });
    UpdateChecker checker(environment,nullptr);
    QString status;
    int available = 0;
    QObject::connect(&checker,&UpdateChecker::Status,&checker,[&](const QString& text) { status = text; });
    QObject::connect(&checker,&UpdateChecker::UpdateAvailable,&checker,[&](const QString&) { ++available; });
    QSettings settings;
    settings.setValue("updates/enabled",false);
    checker.Check();
    Check(requests == 0 && !settings.contains("updates/lastAttempt"),"Opt-out prevents automatic requests");
    checker.Check(true);
    Wait([&] { return available == 1; });
    Check(requests == 1,"Manual check overrides opt-out");
    settings.setValue("updates/enabled",true);
    checker.Check();
    Check(requests == 1,"Daily rate limit after manual check");
    settings.setValue("updates/lastAttempt",QDateTime::currentSecsSinceEpoch() - 86401);
    checker.Check();
    Wait([&] { return available == 2; });
    response = "malformed";
    checker.Check(true);
    Wait([&] { return status.startsWith("Could not"); });
    Check(available == 2,"Malformed response cannot announce an update");
    status = "unchanged";
    settings.setValue("updates/lastAttempt",0);
    checker.Check();
    Wait([&] { return requests == 4; });
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 150) QCoreApplication::processEvents();
    Check(status == "unchanged","Automatic network/parse failures stay quiet");
    stall = true;
    checker.Check(true);
    Wait([&] { return status.startsWith("Could not"); });
    Check(available == 2,"Timeout cannot announce an update");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc,argv);
    try {
        Check(UpdateChecker::Newer("0.1.100","0.1.99"),"Numeric build comparison");
        Check(UpdateChecker::Newer("1.0.102","0.9.101"),"Major bump comparison");
        Check(!UpdateChecker::Newer("0.1.99","0.1.100"),"No downgrade notice");
        Check(!UpdateChecker::Newer("0.1.100","0.1.100"),"No same-version notice");
        for (const QString bad : {"v1.2.3","01.2.3","1.2.3-preview","1.2","999999999999.1.2","<b>1.2.3</b>"})
            Check(!UpdateChecker::Newer(bad,"0.1.0"),"Reject malformed versions");
        Check(UpdateChecker::Target("ID=ubuntu\nVERSION_ID=\"24.04\"\n","x86_64") == "ubuntu-24.04-x86_64","Ubuntu target");
        Check(UpdateChecker::Target("ID=fedora\nVERSION_ID=44\n","x86_64") == "fedora-44-x86_64","Fedora target");
        Check(UpdateChecker::Target("ID=arch\n","x86_64") == "arch-x86_64","Arch target");
        Check(UpdateChecker::Target("ID=debian\nID_LIKE=ubuntu\n","x86_64").isEmpty(),"Do not infer Debian ABI compatibility");
        Check(UpdateChecker::Target("ID=ubuntu\nVERSION_ID=24.04\n","arm64").isEmpty(),"Unsupported architecture");
        const QByteArray json = R"({"schema":1,"channels":{"stable":{"version":"0.1.100","targets":["arch-x86_64"]},"preview":{"version":"0.1.101","targets":["arch-x86_64"]}}})";
        Check(UpdateChecker::Available(json,"stable","arch-x86_64","0.1.99") == "0.1.100","Stable selection");
        Check(UpdateChecker::Available(json,"preview","arch-x86_64","0.1.99") == "0.1.101","Preview selection");
        Check(UpdateChecker::Available(json,"stable","arch-x86_64","0.1.101").isEmpty(),"Preview-to-stable waits");
        Check(UpdateChecker::Available(json,"stable","fedora-44-x86_64","0.1.99").isEmpty(),"Require matching package target");
        Check(UpdateChecker::Available("broken","stable","arch-x86_64","0.1.99").isEmpty(),"Malformed manifest");
        Check(UpdateChecker::Available(QByteArray(300000,' '),"stable","arch-x86_64","0.1.99").isEmpty(),"Response size limit");
        NetworkTests();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
