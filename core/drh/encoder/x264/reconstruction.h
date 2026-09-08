#pragma once

#include "minih264e.h"

namespace barista::drh::x264
{
// Borrowed, deblocked reference picture after a successful IDR/P encode.
// Plane pointers remain valid only until the next encoder call or destruction.
H264E_io_yuv_t Reconstruction(const H264E_persist_t* encoder);
}
