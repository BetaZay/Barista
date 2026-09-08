#pragma once

#include "drh/encoder/h264_cabac.h"

#include <array>
#include <cstddef>

namespace barista::drh
{
inline constexpr size_t H264CabacContextCount = 460;
using H264CabacContexts = std::array<H264CabacState, H264CabacContextCount>;

enum class H264SliceKind
{
    Intra,
    Predicted,
};

// The GamePad's implicit PPS fixes pic_init_qp_minus26 to 6. Predicted
// slices use cabac_init_idc 0, so these contexts always start at QP 32.
H264CabacContexts InitializeGamepadCabacContexts(H264SliceKind kind);
}

