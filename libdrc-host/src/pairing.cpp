#include "drc_host/pairing.h"

#include <array>
#include <sstream>

namespace drc_host
{
namespace
{
constexpr std::array<const char*, 4> kSymbolByDigit{"♠", "♥", "♦", "♣"};

uint8_t constrain_digit(uint8_t value)
{
	return static_cast<uint8_t>(value & 0x03);
}
}

bool PairingCode::valid() const
{
	for (uint8_t digit : digits)
	{
		if (digit > 3)
			return false;
	}
	return true;
}

uint16_t PairingCode::numeric() const
{
	return static_cast<uint16_t>(
		constrain_digit(digits[0]) * 1000 +
		constrain_digit(digits[1]) * 100 +
		constrain_digit(digits[2]) * 10 +
		constrain_digit(digits[3]));
}

uint8_t PairingCode::pin_byte() const
{
	return static_cast<uint8_t>(
		(constrain_digit(digits[0]) << 6) |
		(constrain_digit(digits[1]) << 4) |
		(constrain_digit(digits[2]) << 2) |
		constrain_digit(digits[3]));
}

std::string PairingCode::symbols_utf8(std::string_view separator) const
{
	std::ostringstream out;
	for (size_t i = 0; i < digits.size(); ++i)
	{
		if (i != 0)
			out << separator;
		out << kSymbolByDigit[constrain_digit(digits[i])];
	}
	return out.str();
}

PairingCode PairingCode::from_pin_byte(uint8_t pin_byte)
{
	PairingCode code;
	code.digits[0] = static_cast<uint8_t>((pin_byte >> 6) & 0x03);
	code.digits[1] = static_cast<uint8_t>((pin_byte >> 4) & 0x03);
	code.digits[2] = static_cast<uint8_t>((pin_byte >> 2) & 0x03);
	code.digits[3] = static_cast<uint8_t>(pin_byte & 0x03);
	return code;
}

std::optional<PairingCode> PairingCode::from_numeric(uint16_t value)
{
	PairingCode code;
	code.digits[0] = static_cast<uint8_t>((value / 1000) % 10);
	code.digits[1] = static_cast<uint8_t>((value / 100) % 10);
	code.digits[2] = static_cast<uint8_t>((value / 10) % 10);
	code.digits[3] = static_cast<uint8_t>(value % 10);
	if (!code.valid())
		return std::nullopt;
	return code;
}

std::string pairing_symbol_utf8(PairingSymbol symbol)
{
	return kSymbolByDigit[static_cast<size_t>(symbol)];
}

PairingCode derive_code_from_ap_mac(const MacAddress& ap_mac)
{
	return PairingCode::from_pin_byte(ap_mac.bytes[5]);
}

MacAddress build_pairing_code_mac(const MacAddress& ap_mac, const PairingCode& code)
{
	MacAddress code_mac = ap_mac;
	code_mac.bytes[5] = code.pin_byte();
	return code_mac;
}

std::string build_pairing_ssid(const MacAddress& ap_mac, const PairingCode& code)
{
	const MacAddress code_mac = build_pairing_code_mac(ap_mac, code);
	const std::string code_hex = code_mac.to_hex_no_separator();
	const std::string mac_minus_last_nibble = code_hex.substr(0, code_hex.size() - 1);
	// Captured Wii U pairing APs use the 11-nibble prefix first, followed by
	// the complete MAC: WiiU<mac_minus_last_nibble><mac>_STA1.
	return "WiiU" + mac_minus_last_nibble + code_hex + "_STA1";
}
}
