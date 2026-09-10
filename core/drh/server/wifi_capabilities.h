#pragma once

#include <span>
#include <optional>
#include <string>
#include <string_view>

namespace barista::drh
{
struct WifiApCapabilities
{
	bool ap_mode = false;
	bool five_ghz = false;
	bool usable_pairing_channel = false;
	bool pairing_channel_no_ir = false;
};

WifiApCapabilities AnalyzeWifiApCapabilities(
	std::string_view phy_info, std::span<const int> pairing_channels);

std::optional<std::string> ParseRegulatoryCountry(std::string_view regulatory_info);

bool ShouldRestoreRegulatoryCountry(std::string_view previous_country,
	std::string_view requested_country, std::string_view current_country);
}
