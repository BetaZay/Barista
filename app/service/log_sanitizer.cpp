#include "log_sanitizer.h"

#include <QRegularExpression>

namespace barista
{
QString SanitizeSupportLogLine(QStringView input)
{
    constexpr qsizetype MaximumLineCharacters = 8192;
    QString line = input.left(MaximumLineCharacters).toString();
    if (input.size() > MaximumLineCharacters) line += " <truncated>";

    for (auto& character : line)
    {
        if ((character.unicode() < 0x20 && character != '\t') || character.unicode() == 0x7f)
            character = ' ';
    }

    // Raw hostapd diagnostics contain packet, key, and WPS attribute dumps. The
    // label and length are useful for diagnosis; the bytes themselves are not.
    static const QRegularExpression hexDump(
        R"((?i)(\bhexdump(?:_ascii)?\s*\([^)]*\)\s*:).*$)");
    line.replace(hexDump, "\\1 <redacted>");

    static const QRegularExpression macAddress(
        R"((?i)(?<![0-9a-f])(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}(?![0-9a-f]))");
    line.replace(macAddress, "<mac>");

    static const QRegularExpression ipv4Address(
        R"((?<![0-9.])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9.]))");
    line.replace(ipv4Address, "<ip>");

    static const QRegularExpression ipv6Address(
        R"((?i)(?<![0-9a-f:])(?:[0-9a-f]{1,4}:){2,7}[0-9a-f]{0,4}(?![0-9a-f:]))");
    line.replace(ipv6Address, "<ip>");

    // Pairing and runtime SSIDs contain the AP MAC without separators.
    static const QRegularExpression wiiUSsid(
        R"((?i)WiiU[0-9a-f]{12,28}(?:_STA1)?)");
    line.replace(wiiUSsid, "<ssid>");

    static const QRegularExpression homeDirectory(
        R"((?i)(?<![A-Za-z0-9_])/home/[^/\s]+)");
    line.replace(homeDirectory, "/home/<user>");

    static const QRegularExpression secretAssignment(
        R"((?i)\b(ssid(?:\[[0-9]+\])?|wpa_psk|wpa_passphrase|psk|password|passphrase|network[_ -]?key|pair(?:ing)?[_ -]?code|identity|user(?:name)?)(?=\s*(?:=|:))\s*(?:=|:)\s*(?:"[^"]*"|'[^']*'|[^\s,;]+))");
    line.replace(secretAssignment, "\\1=<redacted>");

    static const QRegularExpression pairStartCode(
        R"((?i)(\bcode)\s*=\s*[0-3]{4}(?=\s|$))");
    line.replace(pairStartCode, "\\1=<redacted>");

    static const QRegularExpression wpsPin(
        R"((?i)(\b(?:wps[_ -]?pin|pin)\b[^0-9\r\n]{0,48})[0-9]{4,8})");
    line.replace(wpsPin, "\\1<redacted>");

    // Catch unlabelled key material while retaining short hashes, channel
    // numbers, driver IDs, and other useful identifiers.
    static const QRegularExpression longHex(
        R"((?i)(?<![0-9a-f])[0-9a-f]{32,}(?![0-9a-f]))");
    line.replace(longHex, "<secret-hex>");

    return line;
}
}
