/* Read-only offline probe of the local patched x264 library.
 * Mirrors drcd's relevant encoder parameters; does not connect to hardware.
 * Compared byte-for-byte with the production worker by test-encoder-reconstruction.py.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x264.h>

struct frame_output {
    uint8_t bytes[4 * 1024 * 1024];
    size_t size;
    int chunks;
    int error;
};

static void collect_chunk(x264_t *encoder, x264_nal_t *nal, void *opaque)
{
    (void)encoder;
    struct frame_output *frame = opaque;
    if (nal->i_type == NAL_SEI)
        return;
    if (nal->i_payload <= 0 || (nal->i_type != NAL_SLICE && nal->i_type != NAL_SLICE_IDR) ||
        (size_t)nal->i_payload > sizeof(frame->bytes) - frame->size) {
        frame->error = 1;
        return;
    }
    memcpy(frame->bytes + frame->size, nal->p_payload, nal->i_payload);
    frame->size += nal->i_payload;
    frame->chunks++;
}

static int stream_frames(x264_t *encoder)
{
    const size_t raw_size = 864 * 480 * 3 / 2;
    uint8_t *raw = malloc(raw_size);
    uint8_t *recon = malloc(raw_size);
    struct frame_output *frame = malloc(sizeof(*frame));
    int result = 1, kind;
    if (!raw || !recon || !frame)
        goto done;
    while ((kind = fgetc(stdin)) != EOF) {
        if (kind > 1 || fread(raw, 1, raw_size, stdin) != raw_size)
            goto done;
        x264_picture_t input, output;
        x264_picture_init(&input);
        x264_picture_init(&output);
        input.i_type = kind ? X264_TYPE_IDR : X264_TYPE_P;
        input.img.i_csp = X264_CSP_I420;
        input.img.i_plane = 3;
        input.img.i_stride[0] = 864;
        input.img.i_stride[1] = input.img.i_stride[2] = 432;
        input.img.plane[0] = raw;
        input.img.plane[1] = raw + 864 * 480;
        input.img.plane[2] = raw + 864 * 480 * 5 / 4;
        frame->size = 0;
        frame->chunks = frame->error = 0;
        input.opaque = frame;
        x264_nal_t *nals;
        int nal_count;
        if (x264_encoder_encode(encoder, &nals, &nal_count, &input, &output) < 0 ||
            frame->error || frame->chunks != 5)
            goto done;
        for (int y = 0; y < 480; y++)
            memcpy(recon + y * 864, output.img.plane[0] + y * output.img.i_stride[0], 864);
        if (output.img.i_csp != X264_CSP_NV12 && output.img.i_csp != X264_CSP_I420) {
            fprintf(stderr, "Unsupported reconstructed format %d\n", output.img.i_csp);
            goto done;
        }
        for (int plane = 1; plane <= 2; plane++) {
            uint8_t *target = recon + 864 * 480 + (plane - 1) * 432 * 240;
            for (int y = 0; y < 240; y++) {
                if (output.img.i_csp == X264_CSP_NV12) {
                    const uint8_t *row = output.img.plane[1] + y * output.img.i_stride[1];
                    for (int x = 0; x < 432; x++)
                        target[y * 432 + x] = row[2 * x + plane - 1];
                } else {
                    memcpy(target + y * 432, output.img.plane[plane] + y * output.img.i_stride[plane], 432);
                }
            }
        }
        uint8_t size[4];
        for (int i = 0; i < 4; i++)
            size[i] = (uint32_t)frame->size >> (8 * i);
        if (fwrite(size, 1, 4, stdout) != 4 ||
            fwrite(frame->bytes, 1, frame->size, stdout) != frame->size ||
            fwrite(recon, 1, raw_size, stdout) != raw_size || fflush(stdout))
            goto done;
    }
    result = ferror(stdin) ? 1 : 0;
done:
    free(raw);
    free(recon);
    free(frame);
    return result;
}

int main(int argc, char **argv)
{
    int compensate = 0, stream = 0, fast = 0;
    int no_psy = 0, no_decimate = 0, no_pskip = 0, trellis2 = 0;
    int zero_deadzone = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "compensate")) compensate = 1;
        else if (!strcmp(argv[i], "stream")) stream = 1;
        else if (!strcmp(argv[i], "fast")) fast = 1;
        else if (!strcmp(argv[i], "no-psy")) no_psy = 1;
        else if (!strcmp(argv[i], "no-decimate")) no_decimate = 1;
        else if (!strcmp(argv[i], "no-pskip")) no_pskip = 1;
        else if (!strcmp(argv[i], "trellis2")) trellis2 = 1;
        else if (!strcmp(argv[i], "zero-deadzone")) zero_deadzone = 1;
        else return 1;
    }
    x264_param_t p, effective;
    if (x264_param_default_preset(&p, "slow", "zerolatency") < 0)
        return 1;
    const char *legacy_quality = getenv("DRCD_LEGACY_ENCODER_QUALITY");
    if (!legacy_quality || strcmp(legacy_quality, "1")) {
        p.analyse.b_fast_pskip = 0;
        p.analyse.b_psy = 0;
    }
    if (fast) {
        p.analyse.i_me_method = X264_ME_DIA;
        p.analyse.i_subpel_refine = 2;
        p.analyse.i_trellis = 0;
    }
    if (no_psy) p.analyse.b_psy = 0;
    if (no_decimate) p.analyse.b_dct_decimate = 0;
    if (no_pskip) p.analyse.b_fast_pskip = 0;
    if (trellis2) p.analyse.i_trellis = 2;
    if (zero_deadzone) p.analyse.i_luma_deadzone[0] = p.analyse.i_luma_deadzone[1] = 0;
    p.i_width = 864;
    p.i_height = 480;
    p.i_csp = X264_CSP_I420;
    p.i_fps_num = 60000;
    p.i_fps_den = 1001;
    p.b_vfr_input = 0;
    p.analyse.inter &= ~X264_ANALYSE_PSUB16x16;
    p.i_keyint_min = 10;
    p.i_keyint_max = 30;
    p.i_scenecut_threshold = -1;
    p.b_cabac = 1;
    p.b_interlaced = 0;
    p.i_bframe = 0;
    p.i_bframe_pyramid = 0;
    p.i_frame_reference = 1;
    p.b_constrained_intra = 1;
    p.b_intra_refresh = 1;
    p.analyse.i_weighted_pred = 0;
    p.analyse.b_weighted_bipred = 0;
    p.analyse.b_transform_8x8 = 0;
    p.analyse.i_chroma_qp_offset = compensate ? 2 : 0;
    p.rc.i_rc_method = X264_RC_CQP;
    p.rc.i_qp_constant = p.rc.i_qp_min = p.rc.i_qp_max = 32;
    p.rc.f_ip_factor = 1.0f;
    p.b_repeat_headers = 0;
    p.b_aud = 0;
    p.b_drh_mode = 1;
    p.i_threads = 1;
    p.b_sliced_threads = 0;
    p.i_slice_count = 1;
    if (stream) {
        p.nalu_process = collect_chunk;
        p.b_full_recon = 1;
    }
    p.i_log_level = X264_LOG_WARNING;
    if (x264_param_apply_profile(&p, "main") < 0)
        return 1;
    int requested_chroma_offset = p.analyse.i_chroma_qp_offset;
    x264_t *encoder = x264_encoder_open(&p);
    if (!encoder)
        return 1;
    x264_encoder_parameters(encoder, &effective);
    fprintf(stream ? stderr : stdout, "{\"requested_chroma_offset\":%d,\"effective_chroma_offset\":%d,"
           "\"psy_rd\":%.2f,\"psy_trellis\":%.2f,\"cabac_init_idc\":%d,"
           "\"deblock\":%d,\"deblock_alpha\":%d,\"deblock_beta\":%d,"
           "\"references\":%d,\"bframes\":%d,\"transform_8x8\":%d,\"fast_pskip\":%d,\"psy\":%d}\n",
           requested_chroma_offset, effective.analyse.i_chroma_qp_offset,
           effective.analyse.f_psy_rd, effective.analyse.f_psy_trellis,
           effective.i_cabac_init_idc, effective.b_deblocking_filter,
           effective.i_deblocking_filter_alphac0, effective.i_deblocking_filter_beta,
           effective.i_frame_reference, effective.i_bframe,
           effective.analyse.b_transform_8x8, effective.analyse.b_fast_pskip,
           effective.analyse.b_psy);
    int result = stream ? stream_frames(encoder) : 0;
    x264_encoder_close(encoder);
    return result;
}
