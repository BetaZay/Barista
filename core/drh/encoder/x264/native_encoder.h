#pragma once

#include "drh/encoder/x264/encoder.h"
#include "drh/encoder/x264/minih264e.h"
#include "drh/encoder/x264/slice.h"
#include "drh/encoder/x264/reconstruction.h"

#include <cstring>
#include <cstdlib>

namespace barista::drh::x264
{
class NativeEncoder final : public VideoEncoder
{
public:
    explicit NativeEncoder(const EncoderOptions& options)
    {
        H264E_create_param_t parameters{};
        parameters.width = DrcVideoWidth;
        parameters.height = DrcVideoHeight;
        parameters.num_layers = 1;
        parameters.const_input_flag = 1;
        parameters.b_drh_mode = 1;
        parameters.disable_planar_prediction_flag = options.disablePlanarPrediction;
        int persistentSize = 0, scratchSize = 0;
        if (H264E_sizeof(&parameters, &persistentSize, &scratchSize))
            return;
        m_encoder = static_cast<H264E_persist_t*>(Allocate(persistentSize));
        m_scratch = static_cast<H264E_scratch_t*>(Allocate(scratchSize));
        m_input = static_cast<unsigned char*>(Allocate(DrcVideoFrameBytes));
        m_valid = m_encoder && m_scratch && m_input && !H264E_init(m_encoder, &parameters);
        // Presets which disable deblocking cannot match the implicit slice header.
        m_speed = options.fastSearch ? 9 : 5;
    }

    ~NativeEncoder() override
    {
        std::free(m_encoder);
        std::free(m_scratch);
        std::free(m_input);
    }

    NativeEncoder(const NativeEncoder&) = delete;
    NativeEncoder& operator=(const NativeEncoder&) = delete;

    bool IsValid() const override { return m_valid; }

    H264E_io_yuv_t ReferencePicture() const
    {
        if (!m_valid || m_firstFrame)
            throw std::logic_error("no reconstructed reference picture");
        return Reconstruction(m_encoder);
    }

    std::optional<EncodedVideoFrame> Encode(std::span<uint8_t> input,
        bool requestIdr, std::string& error) override
    {
        error.clear();
        if (!m_valid || input.size() != DrcVideoFrameBytes)
        {
            error = !m_valid ? "custom H.264 encoder is not initialized" :
                "DRH encoder requires one complete 864x480 I420 picture";
            return std::nullopt;
        }
        const bool idr = requestIdr || m_firstFrame;
        CabacSlice slice(idr);
        std::memcpy(m_input, input.data(), input.size());
        H264E_io_yuv_t picture{{m_input, m_input + DrcVideoWidth * DrcVideoHeight,
            m_input + DrcVideoWidth * DrcVideoHeight * 5 / 4},
            {int(DrcVideoWidth), int(DrcVideoWidth / 2), int(DrcVideoWidth / 2)}};
        H264E_run_param_t run{};
        run.frame_type = idr ? H264E_FRAME_TYPE_KEY : H264E_FRAME_TYPE_P;
        run.encode_speed = m_speed;
        run.qp_min = run.qp_max = 32;
        run.drh_cabac_context = &slice;
        unsigned char* diagnosticOutput = nullptr;
        int diagnosticSize = 0;
        try
        {
            if (H264E_encode(m_encoder, m_scratch, &run, &picture, &diagnosticOutput, &diagnosticSize))
                throw std::runtime_error("MiniH264 frame encoding failed");
            auto frame = slice.Finish(m_frameIndex++);
            m_firstFrame = false;
            return frame;
        }
        catch (const std::exception& exception)
        {
            m_valid = false; // A partially updated reference cannot be reused.
            error = exception.what();
            return std::nullopt;
        }
    }

private:
    static void* Allocate(size_t size) { return std::aligned_alloc(64, (size + 63) & ~size_t(63)); }
    H264E_persist_t* m_encoder = nullptr;
    H264E_scratch_t* m_scratch = nullptr;
    unsigned char* m_input = nullptr;
    bool m_valid = false;
    bool m_firstFrame = true;
    unsigned m_frameIndex = 0;
    int m_speed = 5;
};
}
