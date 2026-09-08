#include "drh/mac_address.h"

#include <cstdio>
#include <string>

namespace barista::drh
{
std::optional<MacAddress> MacAddress::parse(std::string_view text)
{
	unsigned int parsed[6]{};
	if (std::sscanf(std::string{text}.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
		&parsed[0], &parsed[1], &parsed[2], &parsed[3], &parsed[4], &parsed[5]) != 6)
	{
		return std::nullopt;
	}

	MacAddress out;
	for (size_t i = 0; i < out.bytes.size(); ++i)
		out.bytes[i] = static_cast<uint8_t>(parsed[i]);
	return out;
}

std::string MacAddress::to_string() const
{
	char buffer[18]{};
	std::snprintf(buffer, sizeof(buffer), "%02x:%02x:%02x:%02x:%02x:%02x",
		bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
	return std::string{buffer};
}

std::string MacAddress::to_hex_no_separator() const
{
	char buffer[13]{};
	std::snprintf(buffer, sizeof(buffer), "%02x%02x%02x%02x%02x%02x",
		bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
	return std::string{buffer};
}
}
