#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace drc_host
{
struct MacAddress
{
	std::array<uint8_t, 6> bytes{};

	static std::optional<MacAddress> parse(std::string_view text);
	std::string to_string() const;
	std::string to_hex_no_separator() const;

	bool operator==(const MacAddress& other) const = default;
};
}
