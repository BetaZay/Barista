#include "slice.h"

struct H264E_persist_tag;
static void barista_minih264_macroblock(H264E_persist_tag*, int, int);
static unsigned barista_minih264_intra_availability(H264E_persist_tag*);
#define BARISTA_MINIH264_CABAC
#define MINIH264_IMPLEMENTATION
#include "minih264e.h"
#include "reconstruction.h"

H264E_io_yuv_t barista::drh::x264::Reconstruction(const H264E_persist_t* encoder)
{
    return encoder->ref;
}

static unsigned barista_minih264_intra_availability(H264E_persist_tag* enc)
{
    return enc->run_param.drh_cabac_context ?
        static_cast<barista::drh::x264::CabacSlice*>(enc->run_param.drh_cabac_context)->IntraAvailability() : 15;
}

static void barista_minih264_macroblock(H264E_persist_tag* enc, int cbpl, int cbpc)
{
    using namespace barista::drh::x264;
    if (!enc->run_param.drh_cabac_context)
        return;
    CabacMacroblock mb;
    mb.type = enc->mb.type;
    mb.cbp = cbpl | (cbpc << 4);
    if (mb.type >= 5)
    {
        mb.lumaMode = enc->mb.i16.pred_mode_luma;
        mb.chromaMode = mb.lumaMode & 1 ? mb.lumaMode : mb.lumaMode ^ 2;
        std::copy_n(enc->mb.i4x4_mode, 16, mb.intraModes.begin());
    }
    else if (mb.type >= 0)
    {
        const int dx = mb.type & 2 ? 2 : 4;
        const int dy = mb.type & 1 ? 2 : 4;
        int part = 0;
        for (int y = 0; y < 4; y += dy)
            for (int x = 0; x < 4; x += dx, ++part)
                for (int yy = y; yy < y + dy; ++yy)
                    for (int xx = x; xx < x + dx; ++xx)
                        mb.mvd[yy * 4 + xx] = {enc->mb.mvd[part].s.x, enc->mb.mvd[part].s.y};
    }
    // MiniH264's SSE/plain coefficient storage is transposed raster order.
    constexpr unsigned scan[]{0,4,1,2,5,8,12,9,6,3,7,10,13,14,11,15};
    if (mb.type >= 0)
    {
        for (unsigned i = 0; i < 16; ++i)
        {
            if (mb.type >= 6)
                mb.lumaDc[i] = enc->scratch->quant_dc[scan[i]];
            for (unsigned cell = 0; cell < 16; ++cell)
                mb.luma[cell][i] = enc->scratch->qy[cell].qv[scan[i]];
        }
        for (unsigned plane = 0; plane < 2; ++plane)
        {
            const auto* dc = plane ? enc->scratch->quant_dc_v : enc->scratch->quant_dc_u;
            std::copy_n(dc, 4, mb.chromaDc[plane].begin());
            const auto* blocks = plane ? enc->scratch->qv : enc->scratch->qu;
            for (unsigned cell = 0; cell < 4; ++cell)
                for (unsigned i = 1; i < 16; ++i)
                    mb.chromaAc[plane * 4 + cell][i - 1] = blocks[cell].qv[scan[i]];
        }
    }
    static_cast<CabacSlice*>(enc->run_param.drh_cabac_context)->Encode(mb);
}
