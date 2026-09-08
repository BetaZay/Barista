#include "drh/encoder/x264/cabac.h"

#include <array>
#include <stdexcept>

namespace barista::drh::x264
{
namespace
{
constexpr uint8_t RangeLps[64][4] = {
    {2, 2, 2, 2}, {6, 7, 8, 9}, {6, 7, 9, 10}, {6, 8, 9, 11},
    {7, 8, 10, 11}, {7, 9, 10, 12}, {7, 9, 11, 12}, {8, 9, 11, 13},
    {8, 10, 12, 14}, {9, 11, 12, 14}, {9, 11, 13, 15}, {10, 12, 14, 16},
    {10, 12, 15, 17}, {11, 13, 15, 18}, {11, 14, 16, 19}, {12, 14, 17, 20},
    {12, 15, 18, 21}, {13, 16, 19, 22}, {14, 17, 20, 23}, {14, 18, 21, 24},
    {15, 19, 22, 25}, {16, 20, 23, 27}, {17, 21, 25, 28}, {18, 22, 26, 30},
    {19, 23, 27, 31}, {20, 24, 29, 33}, {21, 26, 30, 35}, {22, 27, 32, 37},
    {23, 28, 33, 39}, {24, 30, 35, 41}, {26, 31, 37, 43}, {27, 33, 39, 45},
    {29, 35, 41, 48}, {30, 37, 43, 50}, {32, 39, 46, 53}, {33, 41, 48, 56},
    {35, 43, 51, 59}, {37, 45, 54, 62}, {39, 48, 56, 65}, {41, 50, 59, 69},
    {43, 53, 63, 72}, {46, 56, 66, 76}, {48, 59, 69, 80}, {51, 62, 73, 85},
    {53, 65, 77, 89}, {56, 69, 81, 94}, {59, 72, 86, 99}, {62, 76, 90, 104},
    {66, 80, 95, 110}, {69, 85, 100, 116}, {73, 89, 105, 122}, {77, 94, 111, 128},
    {81, 99, 117, 135}, {85, 104, 123, 142}, {90, 110, 130, 150}, {95, 116, 137, 158},
    {100, 122, 144, 166}, {105, 128, 152, 175}, {111, 135, 160, 185}, {116, 142, 169, 195},
    {123, 150, 178, 205}, {128, 158, 187, 216}, {128, 167, 197, 227}, {128, 176, 208, 240},
};

constexpr uint8_t Transition[128][2] = {
    {0, 0}, {1, 1}, {2, 50}, {51, 3}, {2, 50}, {51, 3}, {4, 52}, {53, 5},
    {6, 52}, {53, 7}, {8, 52}, {53, 9}, {10, 54}, {55, 11}, {12, 54}, {55, 13},
    {14, 54}, {55, 15}, {16, 56}, {57, 17}, {18, 56}, {57, 19}, {20, 56}, {57, 21},
    {22, 58}, {59, 23}, {24, 58}, {59, 25}, {26, 60}, {61, 27}, {28, 60}, {61, 29},
    {30, 60}, {61, 31}, {32, 62}, {63, 33}, {34, 62}, {63, 35}, {36, 64}, {65, 37},
    {38, 66}, {67, 39}, {40, 66}, {67, 41}, {42, 66}, {67, 43}, {44, 68}, {69, 45},
    {46, 68}, {69, 47}, {48, 70}, {71, 49}, {50, 72}, {73, 51}, {52, 72}, {73, 53},
    {54, 74}, {75, 55}, {56, 74}, {75, 57}, {58, 76}, {77, 59}, {60, 78}, {79, 61},
    {62, 78}, {79, 63}, {64, 80}, {81, 65}, {66, 82}, {83, 67}, {68, 82}, {83, 69},
    {70, 84}, {85, 71}, {72, 84}, {85, 73}, {74, 88}, {89, 75}, {76, 88}, {89, 77},
    {78, 90}, {91, 79}, {80, 90}, {91, 81}, {82, 94}, {95, 83}, {84, 94}, {95, 85},
    {86, 96}, {97, 87}, {88, 96}, {97, 89}, {90, 100}, {101, 91}, {92, 100}, {101, 93},
    {94, 102}, {103, 95}, {96, 104}, {105, 97}, {98, 104}, {105, 99}, {100, 108}, {109, 101},
    {102, 108}, {109, 103}, {104, 110}, {111, 105}, {106, 112}, {113, 107}, {108, 114}, {115, 109},
    {110, 116}, {117, 111}, {112, 118}, {119, 113}, {114, 118}, {119, 115}, {116, 122}, {123, 117},
    {118, 122}, {123, 119}, {120, 124}, {125, 121}, {122, 126}, {127, 123}, {124, 127}, {126, 125},
};

constexpr std::array<uint8_t, 64> RenormalizationShift = {
    6, 5, 4, 4, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
}

H264CabacWriter::H264CabacWriter(uint8_t precedingByte)
    : m_buffer{precedingByte}
{
    m_buffer.reserve(64 * 1024);
}

void H264CabacWriter::EncodeDecision(H264CabacState& state, bool bin)
{
    if (m_finished)
        throw std::logic_error("cannot write to a finalized CABAC stream");
    if (state > 127)
        throw std::invalid_argument("invalid H.264 CABAC context state");

    const uint32_t lpsRange = RangeLps[state >> 1U][(m_range >> 6U) - 4U];
    m_range -= lpsRange;
    if (bin != ((state & 1U) != 0))
    {
        m_low += m_range;
        m_range = lpsRange;
    }
    state = Transition[state][bin ? 1 : 0];
    Renormalize();
}

void H264CabacWriter::EncodeBypass(bool bin)
{
    if (m_finished)
        throw std::logic_error("cannot write to a finalized CABAC stream");

    m_low <<= 1U;
    if (bin)
        m_low += m_range;
    ++m_queue;
    PutByte();
}

void H264CabacWriter::EncodeTerminal()
{
    if (m_finished)
        throw std::logic_error("cannot write to a finalized CABAC stream");

    m_range -= 2;
    Renormalize();
}

void H264CabacWriter::Finish(unsigned frameIndex)
{
    if (m_finished)
        throw std::logic_error("CABAC stream is already finalized");

    m_low += m_range - 2U;
    m_low |= 1U;
    m_low <<= 9U;
    m_queue += 9;
    PutByte();
    PutByte();

    m_low <<= static_cast<unsigned>(-m_queue);
    m_low |= ((0x35a4e4f5U >> (frameIndex & 31U)) & 1U) << 10U;
    m_queue = 0;
    PutByte();

    while (m_outstandingBytes > 0)
    {
        m_buffer.push_back(0xff);
        --m_outstandingBytes;
    }
    m_finished = true;
}

size_t H264CabacWriter::BytePosition() const
{
    return m_buffer.size() - 1U + m_outstandingBytes;
}

uint8_t H264CabacWriter::PrecedingByte() const
{
    return m_buffer.front();
}

std::span<const uint8_t> H264CabacWriter::Payload() const
{
    return std::span<const uint8_t>(m_buffer).subspan(1);
}

bool H264CabacWriter::IsFinished() const
{
    return m_finished;
}

void H264CabacWriter::Renormalize()
{
    const unsigned shift = RenormalizationShift[m_range >> 3U];
    m_range <<= shift;
    m_low <<= shift;
    m_queue += static_cast<int>(shift);
    PutByte();
}

void H264CabacWriter::PutByte()
{
    if (m_queue < 0)
        return;

    const uint32_t output = m_low >> static_cast<unsigned>(m_queue + 10);
    m_low &= (0x400U << static_cast<unsigned>(m_queue)) - 1U;
    m_queue -= 8;

    if ((output & 0xffU) == 0xffU)
    {
        ++m_outstandingBytes;
        return;
    }

    const uint8_t carry = static_cast<uint8_t>(output >> 8U);
    m_buffer.back() = static_cast<uint8_t>(m_buffer.back() + carry);
    while (m_outstandingBytes > 0)
    {
        m_buffer.push_back(static_cast<uint8_t>(carry - 1U));
        --m_outstandingBytes;
    }
    m_buffer.push_back(static_cast<uint8_t>(output));
}
}
