#pragma once

#include "drc_host/mac_address.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace drc_host
{
enum class PairingSymbol : uint8_t
{
	Spade = 0,
	Heart = 1,
	Diamond = 2,
	Club = 3
};

struct PairingCode
{
	std::array<uint8_t, 4> digits{};

	bool valid() const;
	uint16_t numeric() const;
	uint8_t pin_byte() const;
	std::string symbols_utf8(std::string_view separator = " ") const;

	static PairingCode from_pin_byte(uint8_t pin_byte);
	static std::optional<PairingCode> from_numeric(uint16_t value);
};

std::string pairing_symbol_utf8(PairingSymbol symbol);
PairingCode derive_code_from_ap_mac(const MacAddress& ap_mac);
MacAddress build_pairing_code_mac(const MacAddress& ap_mac, const PairingCode& code);
std::string build_pairing_ssid(const MacAddress& ap_mac, const PairingCode& code);
}
