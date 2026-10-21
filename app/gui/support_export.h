#pragma once
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

namespace barista
{
inline QString BuildSupportExport(const QString& report, const QString& directory,
    const QString& logName, QString& error)
{
    error.clear();
    if (logName.isEmpty()) return report;
    const QFileInfo selected(QDir(directory).filePath(logName));
    if (directory.isEmpty() || QFileInfo(logName).fileName() != logName ||
        !logName.endsWith(".log") || selected.isSymLink() || !selected.isFile())
    {
        error = "The selected session log is unavailable. Refresh the logs and select a readable support log.";
        return {};
    }
    QFile input(selected.filePath());
    if (!input.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        error = "Could not read the selected session log: " + input.errorString();
        return {};
    }
    constexpr qint64 MaximumBytes = 1024 * 1024;
    const QByteArray contents = input.read(MaximumBytes);
    if (input.error() != QFileDevice::NoError)
    {
        error = "Could not read the selected session log: " + input.errorString();
        return {};
    }
    return report + "\nSelected session log: " + logName +
        "\nThe report above describes the current service; this log describes the selected session.\n\n" +
        QString::fromUtf8(contents) + (input.atEnd() ? QString() : "\n[Log truncated to 1 MiB.]\n");
}
}
