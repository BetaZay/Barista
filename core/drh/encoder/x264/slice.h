#pragma once

#include "drh/encoder/encoder.h"
#include "drh/encoder/x264/macroblock.h"

namespace barista::drh::x264
{
class CabacSlice
{
public:
    explicit CabacSlice(bool idr)
        : m_kind(idr ? H264SliceKind::Intra : H264SliceKind::Predicted),
          m_contexts(InitializeGamepadCabacContexts(m_kind))
    {
    }

    void Encode(CabacMacroblock mb)
    {
        if (m_count >= 1620 || m_writer.IsFinished())
            throw std::logic_error("CABAC slice already contains a complete frame");
        const unsigned x = m_count % 54;
        const auto* left = x ? &m_left : nullptr;
        const auto* top = m_count >= 54 ? &m_top[x] : nullptr;
        if (m_count)
            m_writer.EncodeTerminal();
        EncodeMacroblockHeader(m_writer, m_contexts, m_kind, mb, left, top);
        EncodeMacroblockResidual(m_writer, m_contexts, mb, left, top);
        m_left = mb;
        m_top[x] = mb;
        ++m_count;
        if (m_count % 324 == 0 && m_count < 1620)
            m_ends[m_count / 324 - 1] = m_writer.BytePosition() + 2;
    }

    EncodedVideoFrame Finish(unsigned frameIndex)
    {
        if (m_count != 1620)
            throw std::logic_error("incomplete CABAC frame");
        m_writer.Finish(frameIndex);
        const auto payload = m_writer.Payload();
        if (payload.size() < 5)
            throw std::logic_error("CABAC frame cannot form five chunks");
        EncodedVideoFrame frame{};
        frame.idr = m_kind == H264SliceKind::Intra;
        size_t begin = 0;
        for (size_t i = 0; i < 5; ++i)
        {
            const size_t end = i == 4 ? payload.size() :
                std::clamp(m_ends[i], begin + 1, payload.size() - (4 - i));
            frame.chunks[i] = {
                .bytes = {payload.begin() + begin, payload.begin() + end},
                .nalType = frame.idr ? 5 : 1,
                .referencePriority = 1,
                .firstMacroblock = static_cast<int>(i * 324),
                .lastMacroblock = static_cast<int>((i + 1) * 324 - 1),
            };
            begin = end;
        }
        return frame;
    }

private:
    H264SliceKind m_kind;
    H264CabacContexts m_contexts;
    H264CabacWriter m_writer;
    std::array<CabacMacroblock, 54> m_top{};
    CabacMacroblock m_left;
    std::array<size_t, 4> m_ends{};
    unsigned m_count = 0;
};
}
