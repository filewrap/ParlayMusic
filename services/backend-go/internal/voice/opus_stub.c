//go:build ntgopus_stub

// opus_stub.c — weak no-op fallbacks for the OD-1 Opus C API.
//
// Built ONLY with -tags ntgopus_stub, for a stock
// libntgcalls.so that does not export ntg_opus_set_config /
// ntg_opus_get_config / ntg_opus_set_bitrate: the weak symbols
// let the package link and run (Opus config calls become
// no-ops, the encoder runs at the library defaults) instead
// of failing to load.
//
// The default build MUST NOT compile this file. A weak
// definition in the executable interposes the shared
// library's strong symbol, so with the stub linked the Go
// calls never reach the patched library at all: every Set
// silently succeeds while changing nothing, and every
// readback returns the hardcoded 192000 bps / 60 ms below
// (the Opus round-trip tests fail while the stub is linked).
// With the patched libntgcalls.so (refs/ntgcalls), build
// without the tag and the real symbols are used.
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
