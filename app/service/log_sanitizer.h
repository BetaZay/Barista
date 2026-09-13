#pragma once

#include <QString>
#include <QStringView>

namespace barista
{
QString SanitizeSupportLogLine(QStringView input);
}
