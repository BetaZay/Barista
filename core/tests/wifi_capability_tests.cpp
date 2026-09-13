#include "drh/server/wifi_capabilities.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
void Expect(bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "test failure: " << message << '\n';
		std::exit(1);
	}
}
}

int main()
{
	constexpr std::array pairingChannels{36, 40, 44, 48, 149, 153, 157, 161, 165};
	constexpr std::string_view blocked = R"(
	Supported interface modes:
		 * managed
		 * AP
	Band 2:
		Frequencies:
			* 5180 MHz [36] (22.0 dBm) (no IR)
			* 5200 MHz [40] (22.0 dBm) (no IR)
			* 5745 MHz [149] (22.0 dBm) (no IR)
)";
	const auto blockedCapabilities = barista::drh::AnalyzeWifiApCapabilities(blocked, pairingChannels);
	Expect(blockedCapabilities.ap_mode, "AP mode was not detected");
	Expect(blockedCapabilities.five_ghz, "5 GHz support was not detected");
	Expect(!blockedCapabilities.usable_pairing_channel, "no-IR channel was treated as usable");
	Expect(blockedCapabilities.pairing_channel_no_ir, "regulatory restriction was not detected");

	constexpr std::string_view usable = R"(
	Supported interface modes:
		 * AP
	Band 2:
		Frequencies:
			* 5180 MHz [36] (23.0 dBm)
			* 5200 MHz [40] (disabled)
)";
	const auto usableCapabilities = barista::drh::AnalyzeWifiApCapabilities(usable, pairingChannels);
	Expect(usableCapabilities.usable_pairing_channel, "permitted 5 GHz channel was not detected");
	Expect(!usableCapabilities.pairing_channel_no_ir, "permitted channel was marked no-IR");

	constexpr std::string_view unsupported = R"(
	Supported interface modes:
		 * managed
	Band 1:
		Frequencies:
			* 2412 MHz [1] (20.0 dBm)
)";
	const auto unsupportedCapabilities = barista::drh::AnalyzeWifiApCapabilities(unsupported, pairingChannels);
	Expect(!unsupportedCapabilities.ap_mode, "missing AP mode was accepted");
	Expect(!unsupportedCapabilities.five_ghz, "2.4 GHz was treated as 5 GHz");

	const auto worldCountry = barista::drh::ParseRegulatoryCountry(
		"global\ncountry 00: DFS-UNSET\n");
	Expect(worldCountry && *worldCountry == "00", "world regulatory country was not parsed");
	const auto configuredCountry = barista::drh::ParseRegulatoryCountry(
		"global\ncountry US: DFS-FCC\n");
	Expect(configuredCountry && *configuredCountry == "US", "configured regulatory country was not parsed");
	Expect(!barista::drh::ParseRegulatoryCountry("global\ncountry malformed\n"),
		"malformed regulatory country was accepted");
	Expect(!barista::drh::ParseRegulatoryCountry("global\ncountry U1: DFS-UNSET\n"),
		"mixed alphanumeric regulatory country was accepted");
	Expect(barista::drh::ShouldRestoreRegulatoryCountry("00", "US", "US"),
		"unchanged temporary regulatory country was not restorable");
	Expect(!barista::drh::ShouldRestoreRegulatoryCountry("00", "US", "CA"),
		"an external regulatory country change would be overwritten");
	Expect(!barista::drh::ShouldRestoreRegulatoryCountry("CA", "US", "US"),
		"a configured prior regulatory country would be overwritten");

	std::cout << "Wi-Fi AP capability and no-IR detection passed\n";
}
