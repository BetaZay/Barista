#pragma once

#include <QRandomGenerator>
#include <array>
#include <cstdint>

inline std::array<uint8_t,4> NewPairingPattern(QRandomGenerator& random)
{
    std::array<uint8_t,4> pattern{};
    for (auto& symbol : pattern) symbol = static_cast<uint8_t>(random.bounded(4u));
    return pattern;
}
