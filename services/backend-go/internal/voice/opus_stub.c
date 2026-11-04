#include "ntgcalls.h"
#include "ntgcalls_opus.h"

__attribute__((weak)) ntg_result ntg_opus_set_config(const ntg_opus_encoder_config* config) {
    if (!config) return NTG_ERR_NULL_POINTER;
    return NTG_OK;
}

__attribute__((weak)) ntg_result ntg_opus_get_config(ntg_opus_encoder_config* out) {
    if (!out) return NTG_ERR_NULL_POINTER;
    out->bitrate_bps = 192000;
    out->complexity = 10;
    out->vbr_enabled = true;
    out->fec_enabled = false;
    out->dtx_enabled = false;
    out->frame_size_ms = 60;
    out->max_playback_rate_hz = 48000;
    return NTG_OK;
}

__attribute__((weak)) ntg_result ntg_opus_set_bitrate(int32_t bitrate_bps) {
    return NTG_OK;
}
