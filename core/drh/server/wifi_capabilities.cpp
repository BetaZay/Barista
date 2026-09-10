#include "drh/server/wifi_capabilities.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace barista::drh
{
namespace
{
std::string Lower(std::string_view value)
{
	std::string lower(value);
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return lower;
}

int FrequencyFromLine(std::string_view line)
{
	const size_t mhz = line.find(" MHz");
	if (mhz == std::string_view::npos)
		return -1;
	const size_t begin = line.find_first_of("0123456789");
	if (begin == std::string_view::npos || begin >= mhz)
		return -1;
	try
	{
		return std::stoi(std::string(line.substr(begin, mhz - begin)));
	}
	catch (...)
	{
		return -1;
	}
}

bool ContainsNoIr(std::string_view lower)
{
	return lower.find("no ir") != std::string_view::npos ||
		lower.find("no-ir") != std::string_view::npos ||
		lower.find("no_ir") != std::string_view::npos;
}
}

WifiApCapabilities AnalyzeWifiApCapabilities(
	std::string_view phy_info, std::span<const int> pairing_channels)
{
	WifiApCapabilities result;
	result.ap_mode = phy_info.find("* AP\n") != std::string_view::npos ||
		phy_info.find("* AP\r\n") != std::string_view::npos;

	for (size_t offset = 0; offset < phy_info.size();)
	{
		const size_t end = phy_info.find('\n', offset);
		const std::string_view line = phy_info.substr(offset,
			end == std::string_view::npos ? phy_info.size() - offset : end - offset);
		offset = end == std::string_view::npos ? phy_info.size() : end + 1;

		const int frequency = FrequencyFromLine(line);
		if (frequency < 5000 || frequency >= 5900)
			continue;
		result.five_ghz = true;

		const int channel = (frequency - 5000) / 5;
		if (std::find(pairing_channels.begin(), pairing_channels.end(), channel) == pairing_channels.end())
			continue;
		const std::string lower = Lower(line);
		if (lower.find("disabled") != std::string::npos)
			continue;
		if (ContainsNoIr(lower))
		{
			result.pairing_channel_no_ir = true;
			continue;
		}
		result.usable_pairing_channel = true;
	}
	return result;
}

std::optional<std::string> ParseRegulatoryCountry(std::string_view regulatory_info)
{
	for (size_t offset = 0; offset < regulatory_info.size();)
	{
		const size_t end = regulatory_info.find('\n', offset);
		const std::string_view line = regulatory_info.substr(offset,
			end == std::string_view::npos ? regulatory_info.size() - offset : end - offset);
		offset = end == std::string_view::npos ? regulatory_info.size() : end + 1;
		const size_t marker = line.find("country ");
		if (marker == std::string_view::npos || marker + 10 > line.size())
			continue;
		const std::string_view country = line.substr(marker + 8, 2);
		const bool world = country == "00";
		const bool alpha = std::isalpha(static_cast<unsigned char>(country[0])) &&
			std::isalpha(static_cast<unsigned char>(country[1]));
		if (line[marker + 10] != ':' || (!world && !alpha))
			continue;
		std::string value(country);
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
			return static_cast<char>(std::toupper(ch));
		});
		return value;
	}
	return std::nullopt;
}

bool ShouldRestoreRegulatoryCountry(std::string_view previous_country,
	std::string_view requested_country, std::string_view current_country)
{
	return previous_country == "00" && requested_country != "00" &&
		current_country == requested_country;
}
}
