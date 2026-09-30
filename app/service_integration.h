#pragma once
#include <QString>

namespace barista
{
inline QString ServiceIntegrationName()
{
#ifdef __linux__
    return QString::fromLatin1(BARISTA_INIT_SYSTEM_STRING);
#else
    return QStringLiteral("unavailable");
#endif
}

inline QString ShellQuote(QString value)
{
    value.replace("'", "'\\''");
    return "'" + value + "'";
}

inline QString ServiceRecoveryCommand()
{
#ifdef __linux__
    if (ServiceIntegrationName() == "runit")
        return "sudo " + ShellQuote(BARISTA_RUNIT_SV_PATH) + " -w 10 start " + ShellQuote(BARISTA_RUNIT_SERVICE_PATH);
    if (ServiceIntegrationName() == "openrc")
        return "sudo " + ShellQuote(BARISTA_OPENRC_RC_SERVICE_PATH) + " --ifnotstarted barista start";
    return QStringLiteral("sudo systemctl start barista.service");
#else
    return {};
#endif
}

inline QString SystemPreparationDescription()
{
    return ServiceIntegrationName() == "systemd"
        ? QStringLiteral("Start NetworkManager if needed and load virtual-controller support? This can affect existing network connections.")
        : QStringLiteral("Check that NetworkManager is running and load virtual-controller support? Enable NetworkManager with your distribution's service manager first.");
}
}
