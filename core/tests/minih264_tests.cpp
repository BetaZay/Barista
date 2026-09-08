#include "drh/encoder/x264/minih264e.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{

constexpr int kWidth = 864;
constexpr int kHeight = 480;
constexpr int kLumaSize = kWidth * kHeight;
constexpr int kFrameSize = kLumaSize * 3 / 2;

class AlignedBuffer
{
public:
    explicit AlignedBuffer(std::size_t size)
        : m_size((size + 63U) & ~std::size_t{63U}),
          m_data(static_cast<std::uint8_t *>(std::aligned_alloc(64, m_size)))
    {
        assert(m_data != nullptr);
    }

    ~AlignedBuffer()
    {
        std::free(m_data);
    }

    AlignedBuffer(const AlignedBuffer &) = delete;
    AlignedBuffer &operator=(const AlignedBuffer &) = delete;

    std::uint8_t *Data()
    {
        return m_data;
    }

private:
    std::size_t m_size;
    std::uint8_t *m_data;
};

std::vector<int> NalTypes(const std::uint8_t *data, int size)
{
    std::vector<int> types;
    for (int offset = 0; offset + 4 < size; ++offset)
    {
        int startCodeSize = 0;
        if (data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1)
        {
            startCodeSize = 3;
        }
        else if (offset + 5 < size && data[offset] == 0 && data[offset + 1] == 0 &&
                 data[offset + 2] == 0 && data[offset + 3] == 1)
        {
            startCodeSize = 4;
        }

        if (startCodeSize != 0)
        {
            types.push_back(data[offset + startCodeSize] & 0x1f);
            offset += startCodeSize;
        }
    }
    return types;
}

bool Contains(const std::vector<int> &types, int type)
{
    return std::find(types.begin(), types.end(), type) != types.end();
}

void FillFrame(std::uint8_t *frame, int phase)
{
    for (int y = 0; y < kHeight; ++y)
    {
        for (int x = 0; x < kWidth; ++x)
        {
            frame[y * kWidth + x] = static_cast<std::uint8_t>((x + y + phase) & 0xff);
        }
    }
    std::memset(frame + kLumaSize, 128, kFrameSize - kLumaSize);
}

} // namespace

int main()
{
    H264E_create_param_t create{};
    create.width = kWidth;
    create.height = kHeight;
    create.gop = 30;
    create.const_input_flag = 1;
    create.num_layers = 1;
    create.max_threads = 0;

    int persistentSize = 0;
    int scratchSize = 0;
    assert(H264E_sizeof(&create, &persistentSize, &scratchSize) == H264E_STATUS_SUCCESS);
    assert(persistentSize > 0);
    assert(scratchSize > 0);

    AlignedBuffer persistent(static_cast<std::size_t>(persistentSize));
    AlignedBuffer scratch(static_cast<std::size_t>(scratchSize));
    AlignedBuffer frame(kFrameSize);
    assert(H264E_init(reinterpret_cast<H264E_persist_t *>(persistent.Data()), &create) ==
           H264E_STATUS_SUCCESS);

    H264E_io_yuv_t input{};
    input.yuv[0] = frame.Data();
    input.yuv[1] = frame.Data() + kLumaSize;
    input.yuv[2] = frame.Data() + kLumaSize * 5 / 4;
    input.stride[0] = kWidth;
    input.stride[1] = kWidth / 2;
    input.stride[2] = kWidth / 2;

    H264E_run_param_t run{};
    run.encode_speed = H264E_SPEED_BALANCED;
    run.qp_min = 32;
    run.qp_max = 32;

    unsigned char *encoded = nullptr;
    int encodedSize = 0;
    FillFrame(frame.Data(), 0);
    run.frame_type = H264E_FRAME_TYPE_KEY;
    assert(H264E_encode(reinterpret_cast<H264E_persist_t *>(persistent.Data()),
                        reinterpret_cast<H264E_scratch_t *>(scratch.Data()), &run, &input,
                        &encoded, &encodedSize) == H264E_STATUS_SUCCESS);
    assert(encoded != nullptr);
    assert(encodedSize > 0);
    const std::vector<int> keyTypes = NalTypes(encoded, encodedSize);
    assert(Contains(keyTypes, 7));
    assert(Contains(keyTypes, 8));
    assert(Contains(keyTypes, 5));

    FillFrame(frame.Data(), 3);
    run.frame_type = H264E_FRAME_TYPE_P;
    assert(H264E_encode(reinterpret_cast<H264E_persist_t *>(persistent.Data()),
                        reinterpret_cast<H264E_scratch_t *>(scratch.Data()), &run, &input,
                        &encoded, &encodedSize) == H264E_STATUS_SUCCESS);
    assert(encoded != nullptr);
    assert(encodedSize > 0);
    const std::vector<int> predictedTypes = NalTypes(encoded, encodedSize);
    assert(Contains(predictedTypes, 1));

    return 0;
}
