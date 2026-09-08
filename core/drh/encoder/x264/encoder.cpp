#include "drh/encoder/x264/encoder.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <utility>

extern "C"
{
#include <x264.h>
}

namespace barista::drh::x264
{
namespace
{
bool DefaultEnabled(const char* name)
{
    const char* value = std::getenv(name);
    return !value || std::strcmp(value, "0") != 0;
}

int ConfiguredQuantizer()
{
    const char* selected = std::getenv("DRCD_VIDEO_QP");
    if (selected)
    {
        if (std::strcmp(selected, "28") == 0)
            return 28;
        if (std::strcmp(selected, "32") == 0)
            return 32;
        if (std::strcmp(selected, "36") == 0)
            return 36;
        return 32;
    }

    const char* legacy = std::getenv("DRCD_QP28");
    return legacy && std::strcmp(legacy, "1") == 0 ? 28 : 32;
}

class Encoder final : public VideoEncoder
{
public:
    explicit Encoder(const EncoderOptions& options)
    {
        x264_param_t parameters{};
        x264_param_default_preset(&parameters, "slow", "zerolatency");
        if (options.fastSearch)
        {
            parameters.analyse.i_me_method = X264_ME_DIA;
            parameters.analyse.i_subpel_refine = 2;
            parameters.analyse.i_trellis = 0;
        }
        parameters.i_width = DrcVideoWidth;
        parameters.i_height = DrcVideoHeight;
        parameters.i_csp = X264_CSP_I420;
        parameters.i_fps_num = 60000;
        parameters.i_fps_den = 1001;
        parameters.b_vfr_input = 0;
        parameters.analyse.inter &= ~X264_ANALYSE_PSUB16x16;
        parameters.i_keyint_min = 10;
        parameters.i_keyint_max = 30;
        parameters.i_scenecut_threshold = -1;
        parameters.b_cabac = 1;
        parameters.b_interlaced = 0;
        parameters.i_bframe = 0;
        parameters.i_bframe_pyramid = 0;
        parameters.i_frame_reference = 1;
        parameters.b_constrained_intra = 1;
        parameters.b_intra_refresh = options.intraRefresh ? 1 : 0;
        if (options.preserveReplayFrameTypes && !parameters.b_intra_refresh)
            parameters.i_keyint_max = X264_KEYINT_MAX_INFINITE;
        parameters.analyse.i_weighted_pred = 0;
        parameters.analyse.b_weighted_bipred = 0;
        parameters.analyse.b_transform_8x8 = 0;
        if (!options.legacyQuality)
        {
            parameters.analyse.b_fast_pskip = 0;
            parameters.analyse.b_psy = 0;
        }
        parameters.analyse.i_chroma_qp_offset = 0;
        parameters.rc.i_rc_method = X264_RC_CQP;
        parameters.rc.i_qp_constant = options.quantizer;
        parameters.rc.i_qp_min = options.quantizer;
        parameters.rc.i_qp_max = options.quantizer;
        parameters.rc.f_ip_factor = 1.0f;
        parameters.b_repeat_headers = 0;
        parameters.b_aud = 0;
        parameters.b_drh_mode = 1;
        parameters.i_threads = 1;
        parameters.b_sliced_threads = 0;
        parameters.i_slice_count = 1;
        parameters.nalu_process = ProcessNal;
        parameters.i_log_level = X264_LOG_WARNING;
        x264_param_apply_profile(&parameters, "main");
        m_encoder = x264_encoder_open(&parameters);
        if (m_encoder)
        {
            x264_param_t effective{};
            x264_encoder_parameters(m_encoder, &effective);
            if (effective.analyse.i_chroma_qp_offset != 0)
            {
                x264_encoder_close(m_encoder);
                m_encoder = nullptr;
            }
        }
    }

    ~Encoder() override
    {
        if (m_encoder)
            x264_encoder_close(m_encoder);
    }

    bool IsValid() const override
    {
        return m_encoder != nullptr;
    }

    std::optional<EncodedVideoFrame> Encode(
        std::span<uint8_t> i420,
        bool requestIdr,
        std::string& error) override
    {
        error.clear();
        if (!m_encoder)
        {
            error = "x264 encoder is not initialized";
            return std::nullopt;
        }
        if (i420.size() != DrcVideoFrameBytes)
        {
            error = "DRH encoder requires one complete 864x480 I420 picture";
            return std::nullopt;
        }

        m_chunks = {};
        m_chunkPresent = {};
        m_callbackError.clear();

        x264_picture_t input{};
        x264_picture_init(&input);
        input.opaque = this;
        input.img.i_csp = X264_CSP_I420;
        input.img.i_plane = 3;
        input.img.i_stride[0] = DrcVideoWidth;
        input.img.i_stride[1] = DrcVideoWidth / 2;
        input.img.i_stride[2] = DrcVideoWidth / 2;
        input.img.plane[0] = i420.data();
        input.img.plane[1] = i420.data() + DrcVideoWidth * DrcVideoHeight;
        input.img.plane[2] = input.img.plane[1] + DrcVideoWidth * DrcVideoHeight / 4;
        input.i_type = requestIdr ? X264_TYPE_IDR : X264_TYPE_P;

        x264_nal_t* nals = nullptr;
        int count = 0;
        x264_picture_t output{};
        if (x264_encoder_encode(m_encoder, &nals, &count, &input, &output) < 0)
        {
            error = "x264_encoder_encode failed";
            return std::nullopt;
        }
        if (!m_callbackError.empty())
        {
            error = m_callbackError;
            return std::nullopt;
        }

        const auto chunkCount = std::count(m_chunkPresent.begin(), m_chunkPresent.end(), true);
        if (chunkCount != DrcVideoChunkCount)
        {
            error = "DRH encoder returned " + std::to_string(chunkCount) + " chunks (expected 5)";
            return std::nullopt;
        }

        const bool encodedIdr = std::all_of(m_chunks.begin(), m_chunks.end(), [](const auto& chunk) {
            return chunk.nalType == NAL_SLICE_IDR &&
                chunk.referencePriority != NAL_PRIORITY_DISPOSABLE;
        });
        if (requestIdr && !encodedIdr)
        {
            error = "x264 did not honor the requested IDR frame";
            return std::nullopt;
        }

        return EncodedVideoFrame{
            .idr = encodedIdr,
            .chunks = std::move(m_chunks),
        };
    }

private:
    static void ProcessNal(x264_t*, x264_nal_t* nal, void* opaque)
    {
        if (nal->i_type == NAL_SEI)
            return;

        auto& encoder = *static_cast<Encoder*>(opaque);
        if (nal->i_payload <= 0 || (nal->i_type != NAL_SLICE && nal->i_type != NAL_SLICE_IDR))
        {
            encoder.m_callbackError = "unexpected x264 callback: type=" +
                std::to_string(nal->i_type) + " size=" + std::to_string(nal->i_payload);
            return;
        }

        const int chunkIndex = nal->i_first_mb / static_cast<int>(DrcVideoMacroblocksPerChunk);
        if (chunkIndex < 0 || chunkIndex >= static_cast<int>(DrcVideoChunkCount))
        {
            encoder.m_callbackError = "x264 callback first_mb=" +
                std::to_string(nal->i_first_mb) + " maps outside the five DRH chunks";
            return;
        }
        if (encoder.m_chunkPresent[chunkIndex])
        {
            encoder.m_callbackError = "duplicate x264 callback for DRH chunk " +
                std::to_string(chunkIndex);
            return;
        }
        if (nal->i_first_mb != chunkIndex * static_cast<int>(DrcVideoMacroblocksPerChunk) ||
            nal->i_last_mb != (chunkIndex + 1) * static_cast<int>(DrcVideoMacroblocksPerChunk) - 1)
        {
            encoder.m_callbackError = "DRH callback does not describe exactly six logical rows";
            return;
        }

        encoder.m_chunks[chunkIndex] = {
            .bytes = {nal->p_payload, nal->p_payload + nal->i_payload},
            .nalType = nal->i_type,
            .referencePriority = nal->i_ref_idc,
            .firstMacroblock = nal->i_first_mb,
            .lastMacroblock = nal->i_last_mb,
        };
        encoder.m_chunkPresent[chunkIndex] = true;
    }

    x264_t* m_encoder = nullptr;
    std::array<EncodedVideoChunk, DrcVideoChunkCount> m_chunks{};
    std::array<bool, DrcVideoChunkCount> m_chunkPresent{};
    std::string m_callbackError;
};
}

EncoderOptions OptionsFromEnvironment(bool preserveReplayFrameTypes)
{
    const char* fastSearch = std::getenv("DRCD_FAST_ENCODE");
    const char* legacyQuality = std::getenv("DRCD_LEGACY_ENCODER_QUALITY");
    return {
        .quantizer = ConfiguredQuantizer(),
        .fastSearch = fastSearch && std::strcmp(fastSearch, "1") == 0,
        .intraRefresh = DefaultEnabled("DRCD_INTRA_REFRESH"),
        .legacyQuality = legacyQuality && std::strcmp(legacyQuality, "1") == 0,
        .preserveReplayFrameTypes = preserveReplayFrameTypes,
    };
}

std::unique_ptr<VideoEncoder> CreateEncoder(const EncoderOptions& options)
{
    return std::make_unique<Encoder>(options);
}
}
