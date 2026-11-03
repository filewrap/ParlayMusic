//! C-ABI for Go cgo (see `parlay_dsp.h` for the documented contract).
//!
//! Handles are opaque `uint64_t` ids into a process-wide registry of
//! `Arc<Engine>`. Every entry point clones the `Arc` under a short registry
//! lock and then works on the clone, so `destroy` can never free an engine
//! that another thread is still using: the `Arc` keeps it alive until the
//! in-flight call returns, and `shutdown` wakes any decoder read blocked
//! inside that call.
//!
//! All functions are panic-safe at the boundary: a Rust panic is caught
//! and translated to `PARLAY_DSP_ERR_GENERIC` (a panic must never unwind
//! across the FFI boundary into Go/cgo).
//!
//! Clippy's `missing_safety_doc` is allowed at the crate root: the
//! safety contract for these entry points is `parlay_dsp.h`, the header
//! C/Go callers actually read, and every function documents its pointer
//! requirements inline. Duplicating a `# Safety` section 19 times adds
//! noise, not safety.
//!
//! Threading contract with Go: `push`/`finish_input` run on the
//! feeder goroutine(s); `pull_*` / setters / stats run on the playback
//! goroutine. The engine serialises playback-side calls on its own mutex;
//! `push` never takes it (see `engine.rs`).

#![allow(clippy::missing_safety_doc)]

mod engine;
mod fx;
mod ogg_opus;
mod opus_ffi;
mod params;
mod resample;
mod slot;
mod source;

use engine::{Engine, Stats, PULL_EOS, PULL_NEED_INPUT, PULL_OK};
use std::collections::HashMap;
use std::ffi::{c_char, c_double, c_int, CStr};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;

pub use resample::FRAME_SAMPLES;

/// Return codes (mirrored in `parlay_dsp.h`).
pub const ERR_GENERIC: c_int = -1;
pub const ERR_HANDLE: c_int = -2;
pub const ERR_ARG: c_int = -3;
pub const ERR_DECODE: c_int = -4;

/// ABI version (bump on any incompatible header change).
pub const ABI_VERSION: c_int = 1;

static REGISTRY: std::sync::LazyLock<Mutex<HashMap<u64, std::sync::Arc<Engine>>>> =
    std::sync::LazyLock::new(|| Mutex::new(HashMap::new()));
static NEXT_ID: AtomicU64 = AtomicU64::new(1);

fn lookup(handle: u64) -> Option<std::sync::Arc<Engine>> {
    if handle == 0 {
        return None;
    }
    REGISTRY.lock().unwrap_or_else(|e| e.into_inner()).get(&handle).cloned()
}

fn catch<F>(f: F) -> c_int
where
    F: FnOnce() -> c_int + std::panic::UnwindSafe,
{
    match std::panic::catch_unwind(f) {
        Ok(code) => code,
        Err(_) => ERR_GENERIC,
    }
}

fn is_null<T>(p: *const T) -> bool {
    p.is_null()
}

// --------------------------------------------------------------- lifecycle ---

/// Create an engine. On success writes the handle to `*out_handle` and
/// returns 0; returns `ERR_ARG` if `out_handle` is null.
#[no_mangle]
pub unsafe extern "C" fn parlay_dsp_create(out_handle: *mut u64) -> c_int {
    catch(|| {
        if is_null(out_handle) {
            return ERR_ARG;
        }
        let engine = Engine::new();
        let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
        REGISTRY.lock().unwrap_or_else(|e| e.into_inner()).insert(id, engine);
        unsafe { *out_handle = id };
        0
    })
}

/// Destroy an engine. Safe to call while another thread is blocked in
/// `pull_*`: the blocked call wakes with EOF and the engine is freed when
/// the call returns. Returns 0, or `ERR_HANDLE` for an unknown handle.
#[no_mangle]
pub unsafe extern "C" fn parlay_dsp_destroy(handle: u64) -> c_int {
    catch(|| match REGISTRY.lock().unwrap_or_else(|e| e.into_inner()).remove(&handle) {
        Some(engine) => {
            engine.shutdown();
            0
        }
        None => ERR_HANDLE,
    })
}

/// ABI version.
#[no_mangle]
pub unsafe extern "C" fn parlay_dsp_version() -> c_int {
    ABI_VERSION
}

// ------------------------------------------------------------------ input ---

/// Append encoded bytes to a slot (0 or 1). `len` may be 0 (no-op).
/// Returns 0, `ERR_HANDLE`, or `ERR_ARG`.
#[no_mangle]
pub unsafe extern "C" fn parlay_dsp_push(handle: u64, slot: c_int, data: *const u8, len: usize) -> c_int {
    catch(|| {
        let engine = match lookup(handle) {
            Some(e) => e,
            None => return ERR_HANDLE,
        };
        if len > 0 && is_null(data) {
            return ERR_ARG;
        }
        let bytes = if len == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(data, len) }
        };
        match engine.push(slot, bytes) {
            Ok(()) => 0,
            Err(_) => ERR_ARG,
        }
    })
}

/// Mark a slot's input complete (no more bytes for this track).
#[no_mangle]
pub unsafe extern "C" fn parlay_dsp_finish_input(handle: u64, slot: c_int) -> c_int {
    catch(|| {
        let engine = match lookup(handle) {
            Some(e) => e,
            None => return ERR_HANDLE,
        };
        match engine.finish_input(slot) {
            Ok(()) => 0,
            Err(_) => ERR_ARG,
        }
    })
}

/// Clear a slot for reuse (drops its decoder, clears its bytes). The
/// active slot cannot be reset.
#[no_mangle]
pub unsafe extern "C" fn parlay_dsp_reset_slot(handle: u64, slot: c_int) -> c_int {
    catch(|| {
        let engine = match lookup(handle) {
            Some(e) => e,
            None => return ERR_HANDLE,
        };
        match engine.reset_slot(slot) {
            Ok(()) => 0,
            Err(_) => ERR_ARG,
        }
    })
}

/// Slot index Go may push the next track into (0, 1, or -1 if none free);
/// `ERR_HANDLE` (-2) for an unknown handle.
#[no_mangle]
pub unsafe extern "C" fn parlay_dsp_free_slot(handle: u64) -> c_int {
    catch(|| match lookup(handle) {
        Some(e) => e.free_slot(),
        None => ERR_HANDLE,
    })
}

// ------------------------------------------------------------------- pull ---

/// Pull one 960-frame as interleaved f32 (1920 floats, 48 kHz stereo).
/// Returns `PULL_OK` (0, frame written, `*out_transition` set),
/// `PULL_NEED_INPUT` (1, push more bytes and retry),
/// `PULL_EOS` (2, queue drained), or a negative error code.
/// `*out_transition`: 0 = none, 1 = track boundary crossed in this frame,
/// 2 = loop restart in this frame.
#[no_mangle]
pub unsafe extern "C" fn parlay_dsp_pull_f32(
    handle: u64,
    out_interleaved: *mut f32,
    out_transition: *mut c_int,
) -> c_int {
    catch(|| {
        let engine = match lookup(handle) {
            Some(e) => e,
            None => return ERR_HANDLE,
        };
        if is_null(out_interleaved) || is_null(out_transition) {
            return ERR_ARG;
        }
        let out = unsafe { std::slice::from_raw_parts_mut(out_interleaved, FRAME_SAMPLES * 2) };
        match engine.pull_f32(out) {
            Ok((code, transition)) => {
                unsafe { *out_transition = transition };
                match code {
                    PULL_OK => 0,
                    PULL_NEED_INPUT => 1,
                    PULL_EOS => 2,
                    _ => ERR_GENERIC,
                }
            }
            Err(_) => ERR_DECODE,
        }
    })
}

/// Pull one 960-frame as interleaved s16 (1920 samples, 48 kHz stereo,
/// TPDF-dithered). Same return codes as `parlay_dsp_pull_f32`.
#[no_mangle]
pub unsafe extern "C" fn parlay_dsp_pull_s16(
    handle: u64,
    out_interleaved: *mut i16,
    out_transition: *mut c_int,
) -> c_int {
    catch(|| {
        let engine = match lookup(handle) {
            Some(e) => e,
            None => return ERR_HANDLE,
        };
        if is_null(out_interleaved) || is_null(out_transition) {
            return ERR_ARG;
        }
        let out = unsafe { std::slice::from_raw_parts_mut(out_interleaved, FRAME_SAMPLES * 2) };
        match engine.pull_s16(out) {
            Ok((code, transition)) => {
                unsafe { *out_transition = transition };
                match code {
                    PULL_OK => 0,
                    PULL_NEED_INPUT => 1,
                    PULL_EOS => 2,
                    _ => ERR_GENERIC,
                }
            }
            Err(_) => ERR_DECODE,
        }
    })
}

// ----------------------------------------------------------------- params ---

/// Apply a named preset: "original", "nightcore", "bass-boost",
/// "slow-reverb", "8d". Returns 0 or `ERR_ARG` for an unknown preset.
#[no_mangle]
pub unsafe extern "C" fn parlay_dsp_set_preset(handle: u64, preset: *const c_char) -> c_int {
    catch(|| {
        let engine = match lookup(handle) {
            Some(e) => e,
            None => return ERR_HANDLE,
        };
        if is_null(preset) {
            return ERR_ARG;
        }
        let name = unsafe { CStr::from_ptr(preset) }.to_string_lossy();
        match engine.set_preset(&name) {
            Ok(()) => 0,
            Err(_) => ERR_ARG,
        }
    })
}

/// Playback speed ratio, 0.5–2.0 (clamped). 1.25 = nightcore, 0.8 = slow.
#[no_mangle]
pub unsafe extern "C" fn parlay_dsp_set_speed(handle: u64, speed: c_double) -> c_int {
    catch(|| match lookup(handle) {
        Some(e) => match e.set_speed(speed) {
            Ok(()) => 0,
            Err(_) => ERR_ARG,
        },
        None => ERR_HANDLE,
    })
}

/// Bass shelf gain in dB, -24..+24 (clamped). +10 = bass-boost.
#[no_mangle]
pub unsafe extern "C" fn parlay_dsp_set_bass_db(handle: u64, db: c_double) -> c_int {
    catch(|| match lookup(handle) {
        Some(e) => match e.set_bass_db(db) {
            Ok(()) => 0,
            Err(_) => ERR_ARG,
        },
        None => ERR_HANDLE,
    })
}

/// Volume 0–200 (clamped). 100 = unity, 200 = +12 dB (power law).
#[no_mangle]
pub unsafe extern "C" fn parlay_dsp_set_volume(handle: u64, volume: c_double) -> c_int {
    catch(|| match lookup(handle) {
        Some(e) => match e.set_volume(volume) {
            Ok(()) => 0,
            Err(_) => ERR_ARG,
        },
        None => ERR_HANDLE,
    })
}

/// Loop mode: 0 = off, 1 = loop current track. Other values: `ERR_ARG`.
#[no_mangle]
pub unsafe extern "C" fn parlay_dsp_set_loop(handle: u64, mode: c_int) -> c_int {
    catch(|| match lookup(handle) {
        Some(e) => match e.set_loop(mode) {
            Ok(()) => 0,
            Err(_) => ERR_ARG,
        },
        None => ERR_HANDLE,
    })
}

/// Echo (aecho): enable, delay in ms (1–500, clamped), decay 0–0.95.
#[no_mangle]
pub unsafe extern "C" fn parlay_dsp_set_echo(
    handle: u64,
    enabled: c_int,
    delay_ms: c_double,
    decay: c_double,
) -> c_int {
    catch(|| match lookup(handle) {
        Some(e) => match e.set_echo(enabled != 0, delay_ms, decay) {
            Ok(()) => 0,
            Err(_) => ERR_ARG,
        },
        None => ERR_HANDLE,
    })
}

/// Freeverb reverb: enable, wet level 0–1 (clamped).
#[no_mangle]
pub unsafe extern "C" fn parlay_dsp_set_reverb(handle: u64, enabled: c_int, wet: c_double) -> c_int {
    catch(|| match lookup(handle) {
        Some(e) => match e.set_reverb(enabled != 0, wet) {
            Ok(()) => 0,
            Err(_) => ERR_ARG,
        },
        None => ERR_HANDLE,
    })
}

/// 8D LFO pan + Haas delay: enable/disable.
#[no_mangle]
pub unsafe extern "C" fn parlay_dsp_set_8d(handle: u64, enabled: c_int) -> c_int {
    catch(|| match lookup(handle) {
        Some(e) => match e.set_8d(enabled != 0) {
            Ok(()) => 0,
            Err(_) => ERR_ARG,
        },
        None => ERR_HANDLE,
    })
}

// ------------------------------------------------------------------ stats ---

/// C-visible stats struct. Layout must match `parlay_dsp.h`.
#[repr(C)]
pub struct ParlayDspStats {
    pub state: c_int,
    pub active_slot: c_int,
    pub free_slot: c_int,
    pub loop_mode: c_int,
    pub frames_pulled: u64,
    pub samples_pulled: u64,
    pub transitions: u64,
    pub loops: u64,
    pub underruns: u64,
    pub speed: c_double,
    pub bass_db: c_double,
    pub volume: c_double,
    pub input_sample_rate: c_int,
    pub input_channels: c_int,
}

impl From<Stats> for ParlayDspStats {
    fn from(s: Stats) -> Self {
        ParlayDspStats {
            state: s.state,
            active_slot: s.active_slot,
            free_slot: s.free_slot,
            loop_mode: s.loop_mode,
            frames_pulled: s.frames_pulled,
            samples_pulled: s.samples_pulled,
            transitions: s.transitions,
            loops: s.loops,
            underruns: s.underruns,
            speed: s.speed,
            bass_db: s.bass_db,
            volume: s.volume,
            input_sample_rate: s.input_sample_rate,
            input_channels: s.input_channels,
        }
    }
}

/// Fill `*out` with current stats. Returns 0, `ERR_HANDLE`, or `ERR_ARG`.
#[no_mangle]
pub unsafe extern "C" fn parlay_dsp_stats(handle: u64, out: *mut ParlayDspStats) -> c_int {
    catch(|| {
        let engine = match lookup(handle) {
            Some(e) => e,
            None => return ERR_HANDLE,
        };
        if is_null(out) {
            return ERR_ARG;
        }
        unsafe { *out = ParlayDspStats::from(engine.stats()) };
        0
    })
}

/// Copy the engine's last error message into `buf` (NUL-terminated).
/// Returns bytes written (excluding NUL), 0 if there is no error, or a
/// negative error code. The message is truncated to `buf_len - 1`.
#[no_mangle]
pub unsafe extern "C" fn parlay_dsp_last_error(handle: u64, buf: *mut c_char, buf_len: usize) -> c_int {
    catch(|| {
        let engine = match lookup(handle) {
            Some(e) => e,
            None => return ERR_HANDLE,
        };
        if is_null(buf) || buf_len == 0 {
            return ERR_ARG;
        }
        let msg = engine.last_error();
        let bytes = msg.as_bytes();
        let n = bytes.len().min(buf_len - 1);
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, n);
            *buf.add(n) = 0;
        }
        n as c_int
    })
}
