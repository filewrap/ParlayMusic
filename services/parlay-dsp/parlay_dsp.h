/* parlay_dsp.h — C-ABI for the parlay-dsp Rust engine (Parlay Phase 5).
 *
 * Link: `-lparlay_dsp` (cdylib `libparlay_dsp.so`, also built as rlib
 * `libparlay_dsp.a` for static cgo linking). Runtime dependency:
 * `libopus.so.0` (system libopus, for Ogg-Opus decode).
 *
 * Threading contract:
 *   - push / finish_input: feeder goroutine(s). Never blocks on playback.
 *   - pull_f32 / pull_s16 / setters / stats: playback goroutine.
 *   - A pull may block inside the decoder until more bytes are pushed or
 *     finish_input is called. Push and pull MUST run on different threads.
 *   - destroy is safe while a pull is blocked: the pull wakes with EOF.
 *
 * Output format (both pull functions):
 *   - Exactly 960 samples per channel, 48 kHz, stereo interleaved.
 *   - pull_f32: float  [-1.0, 1.0] (limiter ceiling -0.1 dBFS).
 *   - pull_s16: int16, TPDF-dithered at the final conversion only.
 *   The f32 path never passes through 16-bit; the s16 path is the same
 *   f32 frame plus dither + quantise.
 *
 * Sink format — CONFIRMED 2026-11-03 by Worker A (phase5/worker-a-ntgcalls.md):
 *   ntg_send_external_frame expects s16le, 48 kHz, stereo
 *   (StreamManager::send_external_frame -> AudioStreamer::send_data,
 *   16 bps hardcoded; frame_size() = 1920 bytes per 10 ms frame).
 *   One pull = 960 samples/channel = 3840 bytes s16 = exactly TWO 10 ms
 *   sub-frames of 480 samples (1920 bytes) each. The DSP therefore always
 *   emits exact multiples of the 10 ms frame — but Go (Worker C) MUST chunk
 *   each pull into exact 1920-byte pushes: send_data IGNORES the passed
 *   size and derives the frame count from the configured frame size, so an
 *   oversize buffer is silently truncated and a short one is over-read.
 *   Never send a short final chunk: pad it or drop it on the Go side.
 *
 * Return codes (all functions unless noted otherwise):
 *   0   = ok / frame ready (pull_*)
 *   1   = PULL_NEED_INPUT (pull_*): no frame; push more bytes and retry.
 *   2   = PULL_EOS (pull_*): queue drained (loop off, both slots done).
 *  -1   = ERR_GENERIC   (panic caught at the boundary, or internal error)
 *  -2   = ERR_HANDLE    (unknown handle)
 *  -3   = ERR_ARG       (bad argument: null pointer, bad slot, bad preset,
 *                        bad loop mode, non-finite parameter)
 *  -4   = ERR_DECODE    (pull_*: the active track failed to decode;
 *                        see parlay_dsp_last_error)
 *
 * Gapless / loop model:
 *   - Two slots (0 and 1). Push track N into one slot, track N+1 into the
 *     other. When the active slot drains mid-frame, the engine fills the
 *     rest of the 960-frame from the next slot and reports
 *     *out_transition = 1, so Go can fire the track-advance callback on
 *     the exact sample boundary. No silence is ever inserted.
 *   - parlay_dsp_free_slot() tells Go which slot to push the next track
 *     into (0, 1, or -1 when both are busy).
 *   - Loop mode 1 restarts the ACTIVE track from byte 0 at its end
 *     (transition = 2). Queue loop is Go's job (re-push tracks).
 *
 * FX (all changes are smoothed; no click/pop — see README.md):
 *   - Presets: "original", "nightcore" (speed 1.25), "bass-boost"
 *     (+10 dB @ 110 Hz), "slow-reverb" (speed 0.8 + echo + Freeverb),
 *     "8d" (echo + LFO pan + Haas). A preset never changes volume.
 *   - Speed is a resample-ratio change (pitch AND tempo move together,
 *     vinyl-style). Bass is an RBJ low-shelf biquad. Volume 0-200 maps
 *     to (v/100)^2. The limiter (-0.1 dBFS, 5 ms lookahead) is always on.
 */

#ifndef PARLAY_DSP_H
#define PARLAY_DSP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes. */
#define PARLAY_DSP_OK          0
#define PARLAY_DSP_NEED_INPUT  1
#define PARLAY_DSP_EOS         2
#define PARLAY_DSP_ERR_GENERIC (-1)
#define PARLAY_DSP_ERR_HANDLE  (-2)
#define PARLAY_DSP_ERR_ARG     (-3)
#define PARLAY_DSP_ERR_DECODE  (-4)

/* Output frame geometry. One pull = 960 samples/channel = 20 ms.
 * The ntgcalls sink consumes 10 ms sub-frames (480 samples), so one pull
 * is exactly two sub-frames; see the sink-format note above. */
#define PARLAY_DSP_FRAME_SAMPLES 960
#define PARLAY_DSP_FRAME_SAMPLES_10MS 480
#define PARLAY_DSP_SAMPLE_RATE   48000
#define PARLAY_DSP_CHANNELS      2
/* Bytes per pull / per 10 ms sub-frame on the s16 path. */
#define PARLAY_DSP_S16_FRAME_BYTES 3840
#define PARLAY_DSP_S16_10MS_BYTES  1920

/* Loop modes for parlay_dsp_set_loop. */
#define PARLAY_DSP_LOOP_OFF   0
#define PARLAY_DSP_LOOP_TRACK 1

/* Engine states (ParlayDspStats.state). */
#define PARLAY_DSP_STATE_IDLE       0
#define PARLAY_DSP_STATE_PLAYING    1
#define PARLAY_DSP_STATE_NEED_INPUT 2
#define PARLAY_DSP_STATE_DRAINED    3
#define PARLAY_DSP_STATE_ERROR      4

/* Transition flags (*out_transition on pull_*). */
#define PARLAY_DSP_TRANSITION_NONE      0
#define PARLAY_DSP_TRANSITION_TRACK     1
#define PARLAY_DSP_TRANSITION_LOOP      2

typedef struct {
    int32_t  state;             /* PARLAY_DSP_STATE_* */
    int32_t  active_slot;       /* -1 when idle */
    int32_t  free_slot;         /* -1 when no slot is free */
    int32_t  loop_mode;         /* PARLAY_DSP_LOOP_* */
    uint64_t frames_pulled;     /* 960-sample frames emitted */
    uint64_t samples_pulled;    /* per channel */
    uint64_t transitions;       /* track boundaries crossed */
    uint64_t loops;             /* loop restarts */
    uint64_t underruns;         /* NEED_INPUT returns */
    double   speed;             /* current (smoothed) speed */
    double   bass_db;           /* current (smoothed) bass gain */
    double   volume;            /* volume target 0..200 */
    int32_t  input_sample_rate; /* active track, 0 = unknown */
    int32_t  input_channels;    /* active track, 0 = unknown */
} ParlayDspStats;

/* Lifecycle. */
int      parlay_dsp_create(uint64_t *out_handle);
int      parlay_dsp_destroy(uint64_t handle);
int      parlay_dsp_version(void);

/* Input. `data` may be NULL when `len` is 0. */
int      parlay_dsp_push(uint64_t handle, int slot, const uint8_t *data, size_t len);
int      parlay_dsp_finish_input(uint64_t handle, int slot);
int      parlay_dsp_reset_slot(uint64_t handle, int slot);
int      parlay_dsp_free_slot(uint64_t handle); /* 0, 1, -1 none, -2 bad handle */

/* Pull one frame. `out_interleaved` holds 1920 samples. */
int      parlay_dsp_pull_f32(uint64_t handle, float *out_interleaved, int *out_transition);
int      parlay_dsp_pull_s16(uint64_t handle, int16_t *out_interleaved, int *out_transition);

/* FX. Speed 0.5-2.0 (clamped), bass -24..+24 dB (clamped),
 * volume 0-200 (clamped). Echo delay 1-500 ms (clamped), decay 0-0.95.
 * Reverb wet 0-1 (clamped). */
int      parlay_dsp_set_preset(uint64_t handle, const char *preset);
int      parlay_dsp_set_speed(uint64_t handle, double speed);
int      parlay_dsp_set_bass_db(uint64_t handle, double db);
int      parlay_dsp_set_volume(uint64_t handle, double volume);
int      parlay_dsp_set_loop(uint64_t handle, int mode);
int      parlay_dsp_set_echo(uint64_t handle, int enabled, double delay_ms, double decay);
int      parlay_dsp_set_reverb(uint64_t handle, int enabled, double wet);
int      parlay_dsp_set_8d(uint64_t handle, int enabled);

/* Stats / errors. */
int      parlay_dsp_stats(uint64_t handle, ParlayDspStats *out);
int      parlay_dsp_last_error(uint64_t handle, char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* PARLAY_DSP_H */
