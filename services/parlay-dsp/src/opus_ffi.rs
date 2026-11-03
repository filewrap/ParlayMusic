//! Minimal hand-rolled FFI to the system libopus (`libopus.so.0`).
//!
//! Why not the `opus` / `audiopus` crate? Those go through `opus-sys`, which
//! needs the libopus *headers* and a `libopus.so` linker stub at build time.
//! The sandbox (and the VPS) ships only the runtime library `libopus.so.0`
//! (pulled in by ffmpeg) — no headers, no `.so` symlink, no pkg-config.
//! Declaring the five functions we need ourselves links against the exact
//! runtime filename (`#[link(name = ":libopus.so.0")]`) with zero new crates
//! and zero build-time header requirements.
//!
//! Runtime requirement: `libopus.so.0` must exist where the Go backend runs
//! (it does wherever ffmpeg/libopus is installed; verified in the sandbox).
//!
//! Only the functions the engine uses are bound. Signatures follow
//! `opus.h` (libopus 1.x, stable ABI since 1.0).

use std::ffi::{c_char, c_float, c_int, c_void};

#[link(name = ":libopus.so.0")]
extern "C" {
    fn opus_decoder_create(fs: c_int, channels: c_int, error: *mut c_int) -> *mut c_void;
    fn opus_decoder_destroy(dec: *mut c_void);
    fn opus_decode_float(
        dec: *mut c_void,
        data: *const u8,
        len: c_int,
        pcm: *mut c_float,
        frame_size: c_int,
        decode_fec: c_int,
    ) -> c_int;
    fn opus_strerror(error: c_int) -> *const c_char;
}

/// Safe-ish RAII wrapper around an Opus decoder. The raw pointer is only
/// ever touched while the engine mutex is held, so `Send` is sound here
/// (the engine is otherwise single-threaded per operation).
pub struct OpusDecoder {
    ptr: *mut c_void,
}

// The decoder is only used under the engine lock; it is never shared
// across threads concurrently.
unsafe impl Send for OpusDecoder {}

impl OpusDecoder {
    /// Create a decoder for `sample_rate` Hz / `channels`. libopus only
    /// supports 8/12/16/24/48 kHz; we always decode at 48 kHz.
    pub fn new(sample_rate: u32, channels: u32) -> Result<Self, String> {
        let mut err: c_int = 0;
        let ptr = unsafe {
            opus_decoder_create(sample_rate as c_int, channels as c_int, &mut err)
        };
        if ptr.is_null() || err != 0 {
            return Err(format!("opus_decoder_create failed: {}", opus_error_str(err)));
        }
        Ok(OpusDecoder { ptr })
    }

    /// Decode one Opus packet to interleaved f32 PCM at 48 kHz.
    ///
    /// `data` is one complete Opus packet. `out` must hold at least
    /// `frame_size * channels` floats. Returns the number of samples **per
    /// channel** decoded, or an error string.
    ///
    /// A `None` packet (`data` empty) requests packet-loss concealment;
    /// the engine never does that (a missing packet is an error upstream),
    /// but the binding supports it.
    pub fn decode(
        &mut self,
        data: &[u8],
        out: &mut [f32],
        frame_size: usize,
    ) -> Result<usize, String> {
        let data_ptr = if data.is_empty() {
            std::ptr::null()
        } else {
            data.as_ptr()
        };
        let n = unsafe {
            opus_decode_float(
                self.ptr,
                data_ptr,
                data.len() as c_int,
                out.as_mut_ptr(),
                frame_size as c_int,
                0, // decode_fec = 0
            )
        };
        if n < 0 {
            return Err(format!("opus_decode_float failed: {}", opus_error_str(n)));
        }
        Ok(n as usize)
    }
}

impl Drop for OpusDecoder {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            unsafe { opus_decoder_destroy(self.ptr) };
            self.ptr = std::ptr::null_mut();
        }
    }
}

fn opus_error_str(code: c_int) -> String {
    unsafe {
        let p = opus_strerror(code);
        if p.is_null() {
            return format!("opus error {code}");
        }
        let s = std::ffi::CStr::from_ptr(p).to_string_lossy().into_owned();
        format!("{s} ({code})")
    }
}

/// Maximum Opus frame: 120 ms at 48 kHz.
pub const OPUS_MAX_FRAME_SAMPLES: usize = 5760;
