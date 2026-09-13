#include "log_sanitizer.h"

#include <QStringList>
#include <iostream>

namespace
{
bool Check(const QString& input, const QStringList& retained, const QStringList& removed)
{
    const QString output = barista::SanitizeSupportLogLine(input);
    for (const auto& value : retained)
    {
        if (!output.contains(value))
        {
            std::cerr << "missing retained text: " << value.toStdString() << '\n';
            return false;
        }
    }
    for (const auto& value : removed)
    {
        if (output.contains(value))
        {
            std::cerr << "leaked private text: " << value.toStdString() << '\n';
            return false;
        }
    }
    return true;
}
}

int main()
{
    if (!Check("drcd-backend: adapter-check: wlan0 driver=brcmfmac phy=phy0 ap=yes monitor=no 5ghz=yes pairing-channel=ready",
            {"adapter-check", "wlan0", "brcmfmac", "monitor=no", "pairing-channel=ready"}, {})) return 1;
    if (!Check("hostapd[pairing]: wlan0: AP-STA-CONNECTED aa:bb:cc:dd:ee:ff",
            {"AP-STA-CONNECTED", "<mac>"}, {"aa:bb:cc:dd:ee:ff"})) return 1;
    if (!Check("pair-start: ssid=WiiU00112233445500112233445a_STA1 psk=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
            {"ssid=<redacted>", "psk=<redacted>"}, {"WiiU001122334455", "0123456789abcdef"})) return 1;
    if (!Check("pair-start: iface=wlan0 ap_mac=00:11:22:33:44:55 code=0123 channel=149",
            {"iface=wlan0", "ap_mac=<mac>", "code=<redacted>", "channel=149"},
            {"00:11:22:33:44:55", "code=0123"})) return 1;
    if (!Check("hostapd: ssid[0]=private-network",
            {"ssid[0]=<redacted>"}, {"private-network"})) return 1;
    if (!Check("WPS: Network Key - hexdump_ascii(len=64): 30 31 32 33 34 35",
            {"Network Key", "len=64", "<redacted>"}, {"30 31 32 33"})) return 1;
    if (!Check("hostapd-ctrl: fallback using checksum-corrected pin '01235678'",
            {"checksum-corrected pin", "<redacted>"}, {"01235678"})) return 1;
    if (!Check("DHCP lease active at 192.168.1.11 for 02-11-22-33-44-55 /home/alice/capture",
            {"DHCP lease", "<ip>", "<mac>", "/home/<user>/capture"},
            {"192.168.1.11", "02-11-22-33-44-55", "/home/alice"})) return 1;
    if (!Check("nl80211: Could not configure driver mode: Operation not supported",
            {"nl80211", "Could not configure driver mode", "Operation not supported"}, {})) return 1;
    std::cout << "support log sanitization passed\n";
    return 0;
}
