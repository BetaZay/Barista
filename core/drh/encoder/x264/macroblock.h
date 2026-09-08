#pragma once

#include "drh/encoder/x264/residual.h"

namespace barista::drh::x264
{
// MiniH264 mode numbering; motion differences are expanded to raster 4x4 cells.
struct CabacMacroblock
{
    int type = -1;
    unsigned cbp = 0;
    unsigned lumaMode = 2;
    unsigned chromaMode = 0;
    std::array<int8_t, 16> intraModes{};
    std::array<std::array<int16_t, 2>, 16> mvd{};
    std::array<bool, 27> coded{};
    std::array<int16_t, 16> lumaDc{};
    std::array<std::array<int16_t, 16>, 16> luma{};
    std::array<std::array<int16_t, 4>, 2> chromaDc{};
    std::array<std::array<int16_t, 15>, 8> chromaAc{};
};

inline void EncodeMacroblockHeader(H264CabacWriter& writer, H264CabacContexts& contexts,
    H264SliceKind kind, const CabacMacroblock& mb,
    const CabacMacroblock* left, const CabacMacroblock* top)
{
    auto decision = [&](unsigned index, bool value)
    {
        writer.EncodeDecision(contexts[index], value);
    };
    const bool intraSlice = kind == H264SliceKind::Intra;
    const bool intra = mb.type >= 5;
    if (!intraSlice)
    {
        decision(11 + unsigned(left && left->type != -1) + unsigned(top && top->type != -1), mb.type == -1);
        if (mb.type == -1)
            return;
        decision(14, intra);
    }
    if (intra)
    {
        decision(intraSlice ? 3 + unsigned(left && left->type != 5) + unsigned(top && top->type != 5) : 17,
            mb.type != 5);
        if (mb.type != 5)
        {
            writer.EncodeTerminal(); // I_PCM discriminator, zero for I16x16.
            decision(intraSlice ? 6 : 18, (mb.cbp & 15) != 0);
            decision(intraSlice ? 7 : 19, (mb.cbp >> 4) != 0);
            if (mb.cbp >> 4)
                decision(intraSlice ? 8 : 19, (mb.cbp >> 4) == 2);
            decision(intraSlice ? 9 : 20, (mb.lumaMode >> 1) != 0);
            decision(intraSlice ? 10 : 20, mb.lumaMode & 1);
        }
        else
        {
            constexpr std::array<unsigned, 16> scan{0,1,4,5,2,3,6,7,8,9,12,13,10,11,14,15};
            for (unsigned cell : scan)
            {
                const int mode = mb.intraModes[cell];
                decision(68, mode < 0);
                if (mode >= 0)
                    for (unsigned bit = 0; bit < 3; ++bit)
                        decision(69, (mode >> bit) & 1);
            }
        }
        decision(64 + unsigned(left && left->chromaMode) + unsigned(top && top->chromaMode), mb.chromaMode != 0);
        if (mb.chromaMode)
        {
            decision(67, mb.chromaMode > 1);
            if (mb.chromaMode > 1)
                decision(67, mb.chromaMode > 2);
        }
    }
    else
    {
        decision(15, mb.type == 1 || mb.type == 2);
        decision(mb.type == 1 || mb.type == 2 ? 17 : 16, mb.type == 1 || mb.type == 3);
        if (mb.type == 3)
            for (unsigned part = 0; part < 4; ++part)
                decision(21, true); // P_L0_8x8 subpartitions.
        const unsigned width = (mb.type & 2) ? 2 : 4;
        const unsigned height = (mb.type & 1) ? 2 : 4;
        for (unsigned y = 0; y < 4; y += height)
            for (unsigned x = 0; x < 4; x += width)
                for (unsigned axis = 0; axis < 2; ++axis)
                {
                    const int a = x ? mb.mvd[y * 4 + x - 1][axis] : left ? left->mvd[y * 4 + 3][axis] : 0;
                    const int b = y ? mb.mvd[(y - 1) * 4 + x][axis] : top ? top->mvd[12 + x][axis] : 0;
                    const unsigned sum = std::abs(a) + std::abs(b);
                    const int value = mb.mvd[y * 4 + x][axis];
                    const unsigned magnitude = std::abs(value);
                    const unsigned base = axis ? 47 : 40;
                    decision(base + (sum > 32 ? 2 : sum > 2 ? 1 : 0), magnitude != 0);
                    if (!magnitude)
                        continue;
                    for (unsigned bin = 1; bin < std::min(magnitude, 9U); ++bin)
                        decision(base + std::min(6U, bin + 2), true);
                    if (magnitude < 9)
                        decision(base + std::min(6U, magnitude + 2), false);
                    else
                    {
                        unsigned suffix = magnitude - 9, order = 3;
                        while (suffix >= (1U << order))
                        {
                            writer.EncodeBypass(true);
                            suffix -= 1U << order++;
                        }
                        writer.EncodeBypass(false);
                        while (order)
                            writer.EncodeBypass((suffix >> --order) & 1U);
                    }
                    writer.EncodeBypass(value < 0);
                }
    }
    if (mb.type < 6)
    {
        const unsigned l = left ? left->cbp : 15;
        const unsigned t = top ? top->cbp : 15;
        decision(73 + unsigned(!(l & 2)) + 2 * unsigned(!(t & 4)), mb.cbp & 1);
        decision(73 + unsigned(!(mb.cbp & 1)) + 2 * unsigned(!(t & 8)), mb.cbp & 2);
        decision(73 + unsigned(!(l & 8)) + 2 * unsigned(!(mb.cbp & 1)), mb.cbp & 4);
        decision(73 + unsigned(!(mb.cbp & 4)) + 2 * unsigned(!(mb.cbp & 2)), mb.cbp & 8);
        decision(77 + unsigned((l >> 4) != 0) + 2 * unsigned((t >> 4) != 0), mb.cbp >> 4);
        if (mb.cbp >> 4)
            decision(81 + unsigned((l >> 4) == 2) + 2 * unsigned((t >> 4) == 2), (mb.cbp >> 4) == 2);
    }
    if (mb.cbp || mb.type >= 6)
        decision(60, false); // Fixed QP32: mb_qp_delta is always zero.
}

inline void EncodeMacroblockResidual(H264CabacWriter& writer, H264CabacContexts& contexts,
    CabacMacroblock& mb, const CabacMacroblock* left, const CabacMacroblock* top)
{
    mb.coded.fill(false);
    if (mb.type == -1)
        return;
    const bool intra = mb.type >= 5;
    auto block = [&](ResidualCategory category, unsigned slot, std::span<const int16_t> values,
                     bool a, bool b)
    {
        EncodeResidual(writer, contexts, category, values, a, b);
        mb.coded[slot] = std::any_of(values.begin(), values.end(), [](int16_t value) { return value != 0; });
    };
    if (mb.type >= 6)
        block(ResidualCategory::LumaDc, 24, mb.lumaDc,
            left ? left->coded[24] : intra, top ? top->coded[24] : intra);
    constexpr std::array<unsigned, 16> scan{0,1,4,5,2,3,6,7,8,9,12,13,10,11,14,15};
    for (unsigned i = 0; i < 16; ++i)
    {
        const unsigned cell = scan[i];
        const unsigned x = cell % 4, y = cell / 4;
        if (!(mb.cbp & (1U << (i / 4))))
            continue;
        const bool a = x ? mb.coded[cell - 1] : left ? left->coded[y * 4 + 3] : intra;
        const bool b = y ? mb.coded[cell - 4] : top ? top->coded[12 + x] : intra;
        const auto values = std::span<const int16_t>(mb.luma[cell]);
        block(mb.type >= 6 ? ResidualCategory::LumaAc : ResidualCategory::Luma4x4,
            cell, mb.type >= 6 ? values.subspan(1) : values, a, b);
    }
    if (!(mb.cbp >> 4))
        return;
    for (unsigned plane = 0; plane < 2; ++plane)
        block(ResidualCategory::ChromaDc, 25 + plane, mb.chromaDc[plane],
            left ? left->coded[25 + plane] : intra, top ? top->coded[25 + plane] : intra);
    if ((mb.cbp >> 4) != 2)
        return;
    for (unsigned plane = 0; plane < 2; ++plane)
        for (unsigned cell = 0; cell < 4; ++cell)
        {
            const unsigned base = 16 + plane * 4;
            const unsigned x = cell % 2, y = cell / 2;
            const bool a = x ? mb.coded[base + cell - 1] : left ? left->coded[base + y * 2 + 1] : intra;
            const bool b = y ? mb.coded[base + cell - 2] : top ? top->coded[base + 2 + x] : intra;
            block(ResidualCategory::ChromaAc, base + cell, mb.chromaAc[plane * 4 + cell], a, b);
        }
}
}
