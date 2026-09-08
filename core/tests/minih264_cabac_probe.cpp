#include "drh/encoder/x264/slice.h"
#include "drh/encoder/x264/bit_writer.h"
#include <fstream>
#include "drh/encoder/x264/minih264e.h"
#include <cstdlib>
#include <cstring>
#include <stdexcept>
int main(int argc, char** argv)
{
    if (argc != 3)
        throw std::runtime_error("expected CAVLC and CABAC output paths");
    using namespace barista::drh::x264;
    H264E_create_param_t create{};
    create.width = 864; create.height = 480;
    create.num_layers = 1; create.const_input_flag = 1;
    create.b_drh_mode = 1; create.disable_planar_prediction_flag = 1;
    int persistentSize, scratchSize;
    if (H264E_sizeof(&create, &persistentSize, &scratchSize))
        throw std::runtime_error("invalid encoder parameters");
    auto* enc = static_cast<H264E_persist_t*>(std::aligned_alloc(64, (persistentSize + 63) & ~63));
    auto* scratch = static_cast<H264E_scratch_t*>(std::aligned_alloc(64, (scratchSize + 63) & ~63));
    if (!enc || !scratch || H264E_init(enc, &create))
        throw std::runtime_error("encoder initialization failed");
    std::vector<unsigned char> pixels(864 * 480 * 3 / 2, 128);
    std::ofstream baseline(argv[1], std::ios::binary);
    std::ofstream out(argv[2], std::ios::binary);
    baseline.exceptions(std::ios::badbit | std::ios::failbit);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    unsigned number = 0;
    for (unsigned index = 0; index < 90; ++index)
    {
    const bool idr = index == 0 || index == 45;
    if (idr) number = 0;
    CabacSlice slice(idr);
    for (int y = 0; y < 480; ++y)
        for (int x = 0; x < 864; ++x)
            pixels[y * 864 + x] = 16 + ((x + y + index * 3) % 220);
    H264E_io_yuv_t input{{pixels.data(), pixels.data() + 864 * 480, pixels.data() + 864 * 480 * 5 / 4}, {864,432,432}};
    H264E_run_param_t run{};
    run.encode_speed = 5; run.frame_type = idr ? H264E_FRAME_TYPE_KEY : H264E_FRAME_TYPE_P;
    run.qp_min = run.qp_max = 32;
    run.drh_cabac_context = &slice;
    unsigned char* coded; int size;
    if (H264E_encode(enc, scratch, &run, &input, &coded, &size))
        throw std::runtime_error("encode failed");
    baseline.write(reinterpret_cast<char*>(coded), size);
    auto frame = slice.Finish(index);
    const uint32_t header = idr ? 0x25b804ff : 0x21e003ff | ((number & 255) << 13);
    std::vector<uint8_t> data{uint8_t(header >> 24),uint8_t(header >> 16),uint8_t(header >> 8),uint8_t(header)};
    for (auto& chunk : frame.chunks)
        data.insert(data.end(), chunk.bytes.begin(), chunk.bytes.end());
    auto escaped = EscapeH264Rbsp(data);
    const unsigned char headers[]{0,0,0,1,0x67,0x64,0,0x20,0xac,0x2b,0x40,0x6c,0x1e,0xf3,0x68,
        0,0,0,1,0x68,0xee,0x06,0x0c,0xe8,0,0,0,1};
    if (idr) out.write(reinterpret_cast<const char*>(headers), sizeof(headers));
    else out.write("\0\0\0\1", 4);
    out.write(reinterpret_cast<const char*>(escaped.data()), escaped.size());
    ++number;
    }
    std::free(enc); std::free(scratch);
}
