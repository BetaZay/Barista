#pragma once

#include "drh/encoder/x264/contexts.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <stdexcept>

namespace barista::drh::x264
{
enum class ResidualCategory : unsigned
{
    LumaDc, LumaAc, Luma4x4, ChromaDc, ChromaAc
};

// H.264 9.3.2/9.3.3: coefficients are supplied in scan order, with DC
// omitted for AC categories. Neighbor flags must account for availability.
inline void EncodeResidual(H264CabacWriter& writer, H264CabacContexts& contexts,
    ResidualCategory category, std::span<const int16_t> coefficients,
    bool leftCoded, bool topCoded)
{
    constexpr std::array<unsigned, 5> counts{16, 15, 16, 4, 15};
    constexpr std::array<unsigned, 5> significant{105, 120, 134, 149, 152};
    constexpr std::array<unsigned, 5> last{166, 181, 195, 210, 213};
    constexpr std::array<unsigned, 5> levels{227, 237, 247, 257, 266};
    const unsigned cat = static_cast<unsigned>(category);
    if (cat >= counts.size() || coefficients.size() != counts[cat])
        throw std::invalid_argument("invalid CABAC residual category or block size");
    auto decision = [&](unsigned context, bool bin)
    {
        writer.EncodeDecision(contexts[context], bin);
    };
    int lastNonzero = -1;
    for (unsigned i = 0; i < coefficients.size(); ++i)
        if (coefficients[i])
            lastNonzero = static_cast<int>(i);
    decision(85 + 4 * cat + unsigned(leftCoded) + 2 * unsigned(topCoded), lastNonzero >= 0);
    if (lastNonzero < 0)
        return;
    for (int i = 0; i <= lastNonzero && i < static_cast<int>(coefficients.size()) - 1; ++i)
    {
        decision(significant[cat] + i, coefficients[i] != 0);
        if (coefficients[i])
            decision(last[cat] + i, i == lastNonzero);
    }
    unsigned equalOne = 0;
    unsigned greaterOne = 0;
    for (int i = lastNonzero; i >= 0; --i)
    {
        const int value = coefficients[i];
        if (!value)
            continue;
        const unsigned magnitude = static_cast<unsigned>(std::abs(value));
        decision(levels[cat] + (greaterOne ? 0 : std::min(4U, equalOne + 1)), magnitude > 1);
        if (magnitude > 1)
        {
            const unsigned context = levels[cat] + 5 +
                std::min(cat == 3 ? 3U : 4U, greaterOne);
            for (unsigned bin = 2; bin < std::min(magnitude, 15U); ++bin)
                decision(context, true);
            if (magnitude < 15)
                decision(context, false);
            else
            {
                unsigned suffix = magnitude - 15;
                unsigned order = 0;
                while (suffix >= (1U << order))
                {
                    writer.EncodeBypass(true);
                    suffix -= 1U << order++;
                }
                writer.EncodeBypass(false);
                while (order)
                    writer.EncodeBypass((suffix >> --order) & 1U);
            }
            ++greaterOne;
        }
        else
            ++equalOne;
        writer.EncodeBypass(value < 0);
    }
}
}
