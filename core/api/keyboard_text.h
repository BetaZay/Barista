#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace barista::api
{
// Strict UTF-8 validation also provides scalar boundaries for editing/rendering.
inline bool Utf8Characters(std::string_view text, std::vector<uint32_t>& characters)
{
    characters.clear();
    for (size_t offset = 0; offset < text.size();)
    {
        const auto first = static_cast<uint8_t>(text[offset++]);
        uint32_t value = first;
        unsigned remaining = 0;
        uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { remaining = 1; value &= 31; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { remaining = 2; value &= 15; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { remaining = 3; value &= 7; minimum = 0x10000; }
        else if (first >= 0x80) return false;
        if (offset + remaining > text.size()) return false;
        for (unsigned index = 0; index < remaining; ++index)
        {
            const auto byte = static_cast<uint8_t>(text[offset++]);
            if ((byte & 0xc0) != 0x80) return false;
            value = (value << 6) | (byte & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
            value < 32 || value == 127) return false;
        characters.push_back(value);
    }
    return true;
}
inline void EraseLastCharacter(std::string& text)
{
    if (text.empty()) return;
    size_t offset = text.size() - 1;
    while (offset > 0 && (static_cast<uint8_t>(text[offset]) & 0xc0) == 0x80) --offset;
    text.resize(offset);
}
}
