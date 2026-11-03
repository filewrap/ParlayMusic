//! Minimal in-process ntgcalls FFI — std-only, no new crates.
//!
//! ntgcalls is a C++ NATIVE LIBRARY. The refs/ntgcalls repo ships a C
//! target (`targets/c`) producing `libntgcalls.so` with the `ntg_*` C
//! API (the same header core-cpp keeps as
//! `services/core-cpp/src/ntgcalls.h`), and a Rust target
//! (`targets/rust`: `ntgcalls` + `ntgcalls-sys` crates) that is
//! pure-codegen over PREBUILT static libs published to crates.io —
//! final-only, no local build. Depending on `ntgcalls-sys` would add a
//! crates.io dependency plus a prebuilt-binary download, and vc-rust
//! builds offline (`cargo --offline`). So this module does the smallest
//! honest thing instead: it dlopens `libntgcalls.so` at runtime with
//! the libc `dlopen`/`dlsym` symbols (declared here as `extern "C"` —
//! on glibc ≥ 2.34 they live in libc itself, no `-ldl`, no crates)
//! and resolves only the `ntg_*` functions voice.rs needs.
//!
//! The library is loaded lazily and only when a voice call actually
//! needs it; when the .so is absent `NtgLib::load()` returns the exact
//! dlopen failure and the caller falls back to the ntgcalls-wheel
//! probe (`src/ntg_offer.py`) for the local offer — the media side
//! stays honest, and the MTProto join is delegated to the core
//! (see voice.rs: no MTProto client in Rust stdlib; the join lives in
//! core-cpp's TdLibVoiceCall, reached over POST {CORE_URL}/v1/join).
//!
//! API shape: ntgcalls 3.0.5 is SYNCHRONOUS (generated header
//! refs/ntgcalls/shared-output/include/ntgcalls.h). Every call returns
//! `ntg_result` directly (NTG_OK = 0); the error text comes from
//! `ntg_last_error()` and out strings are freed with
//! `ntg_string_free()`. No `ntg_async_struct` any more — the old
//! condvar machinery was deleted with the API change (phase5).

use std::ffi::{c_char, c_int, c_void, CString};

// libc dlopen/dlsym/dlclose/dlerror (glibc >= 2.34: in libc itself).
extern "C" {
    fn dlopen(filename: *const c_char, flag: c_int) -> *mut c_void;
    fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
    fn dlclose(handle: *mut c_void) -> c_int;
    fn dlerror() -> *const c_char;
}

const RTLD_NOW: c_int = 2;
const RTLD_GLOBAL: c_int = 0x100;

// --- ntgcalls 3.0.5 C API (subset of the generated ntgcalls.h) ---

// ntg_result values we report; NTG_OK = 0, negatives are errors.
pub const NTG_OK: i32 = 0;

// Media source enum (ntgcalls.h): UNKNOWN=0, FILE=1, SHELL=2,
// FFMPEG=3, DEVICE=4, DESKTOP=5, EXTERNAL=6.
#[allow(dead_code)]
pub const NTG_MEDIA_SOURCE_SHELL: i32 = 2;
#[allow(dead_code)]
pub const NTG_STREAM_MODE_CAPTURE: i32 = 0;

// Field order is the NEW header's: media_source, sample_rate,
// channel_count, input, keep_open (the old header had input second).
#[repr(C)]
pub struct NtgAudioDesc {
    pub media_source: c_int,
    pub sample_rate: u32,
    pub channel_count: u8,
    pub input: *mut c_char,
    pub keep_open: bool,
}

#[repr(C)]
pub struct NtgVideoDesc {
    pub media_source: c_int,
    pub width: i16,
    pub height: i16,
    pub fps: u8,
    pub input: *mut c_char,
    pub keep_open: bool,
}

#[repr(C)]
pub struct NtgMediaDesc {
    pub microphone: *const NtgAudioDesc,
    pub speaker: *const NtgAudioDesc,
    pub camera: *const NtgVideoDesc,
    pub screen: *const NtgVideoDesc,
}

type FnInstanceCreate = extern "C" fn() -> *mut c_void;
type FnInstanceDestroy = extern "C" fn(*mut c_void);
type FnCreateCall = extern "C" fn(*mut c_void, i64, *mut *mut c_char) -> i32;
type FnConnect = extern "C" fn(*mut c_void, i64, *const c_char, bool) -> i32;
type FnSetSources =
    extern "C" fn(*mut c_void, i64, i32, NtgMediaDesc) -> i32;
type FnBoolOut = extern "C" fn(*mut c_void, i64, *mut bool) -> i32;
type FnStop = extern "C" fn(*mut c_void, i64) -> i32;
type FnTime = extern "C" fn(*mut c_void, i64, i32, *mut u64) -> i32;
type FnGetVersion = extern "C" fn() -> *const c_char;
type FnLastError = extern "C" fn() -> *const c_char;
type FnStringFree = extern "C" fn(*mut c_char);

/// A loaded libntgcalls.so + one ntg instance. `Send` only via the
/// caller's own locking (voice.rs holds it behind the RealVoiceCall
/// state); the raw handle is never shared across threads here.
/// The connect/set_sources/pause/resume/mute/unmute/time/version
/// methods are the public API for the day a Rust MTProto client exists
/// (today only create_offer is called); they are intentionally kept
/// and silenced rather than deleted.
#[allow(dead_code)]
pub struct NtgLib {
    handle: *mut c_void,
    ntg: *mut c_void,
    lib_path: String,
    instance_create: FnInstanceCreate,
    instance_destroy: FnInstanceDestroy,
    create_call: FnCreateCall,
    connect: FnConnect,
    set_stream_sources: FnSetSources,
    stop: FnStop,
    pause: FnBoolOut,
    resume: FnBoolOut,
    mute: FnBoolOut,
    unmute: FnBoolOut,
    time: FnTime,
    get_version: Option<FnGetVersion>,
    last_error: Option<FnLastError>,
    string_free: Option<FnStringFree>,
}

// NtgLib is used from one thread at a time in voice.rs (the join/play
// paths are synchronous); the raw dlopen handle is only ever closed
// in Drop. Marking it Send lets RealVoiceCall hold it in a Mutex.
unsafe impl Send for NtgLib {}

#[allow(dead_code)]
impl NtgLib {
    /// Candidate .so paths, same order as core-cpp NtgCallsVoiceCall
    /// plus the ntgcalls C-build output dir; NTGCALLS_LIB wins.
    pub fn candidates() -> Vec<String> {
        let mut out = Vec::new();
        if let Ok(p) = std::env::var("NTGCALLS_LIB") {
            if !p.is_empty() {
                out.push(p);
            }
        }
        // C-build output (refs/ntgcalls/shared-output/lib): first the
        // service-dir relative path, then the repo-root relative one.
        for rel in [
            "../../refs/ntgcalls/shared-output/lib/libntgcalls.so",
            "refs/ntgcalls/shared-output/lib/libntgcalls.so",
            "refs/ntgcalls/build-c/shared-output/lib/libntgcalls.so",
        ] {
            out.push(rel.to_string());
        }
        out.push("./lib/libntgcalls.so".to_string());
        out.push("/usr/local/lib/libntgcalls.so".to_string());
        out.push("/usr/lib/libntgcalls.so".to_string());
        out.push("/usr/lib/x86_64-linux-gnu/libntgcalls.so".to_string());
        out.push("libntgcalls.so".to_string());
        out
    }

    /// First candidate that exists on disk, if any.
    pub fn first_existing() -> Option<String> {
        Self::candidates()
            .into_iter()
            .find(|p| std::path::Path::new(p).exists())
    }

    /// dlopen the first loadable candidate (or `explicit` when given)
    /// and resolve the ntg_* symbols. Err carries the exact dlopen /
    /// symbol failure — never a silent unavailable.
    pub fn load(explicit: Option<&str>) -> Result<NtgLib, String> {
        let mut paths: Vec<String> = Vec::new();
        if let Some(p) = explicit {
            if !p.is_empty() {
                paths.push(p.to_string());
            }
        }
        paths.extend(Self::candidates());
        let mut last_err = String::new();
        for path in &paths {
            let cpath = match CString::new(path.as_str()) {
                Ok(c) => c,
                Err(_) => continue,
            };
            let handle = unsafe { dlopen(cpath.as_ptr(), RTLD_NOW | RTLD_GLOBAL) };
            if handle.is_null() {
                let e = unsafe {
                    let p = dlerror();
                    if p.is_null() {
                        String::new()
                    } else {
                        std::ffi::CStr::from_ptr(p).to_string_lossy().to_string()
                    }
                };
                last_err = format!("dlopen {} failed: {}", path, e);
                continue;
            }
            match unsafe { Self::from_handle(handle, path.clone()) } {
                Ok(lib) => return Ok(lib),
                Err(e) => {
                    unsafe { dlclose(handle) };
                    last_err = e;
                    continue;
                }
            }
        }
        Err(if last_err.is_empty() {
            "libntgcalls.so not found (no candidate path exists)".to_string()
        } else {
            last_err
        })
    }

    unsafe fn from_handle(handle: *mut c_void, path: String) -> Result<NtgLib, String> {
        unsafe fn sym<T>(handle: *mut c_void, name: &str) -> Option<T> {
            let cname = CString::new(name).ok()?;
            let p = dlsym(handle, cname.as_ptr());
            if p.is_null() {
                None
            } else {
                Some(std::mem::transmute_copy::<*mut c_void, T>(&p))
            }
        }
        let instance_create: FnInstanceCreate =
            match unsafe { sym(handle, "ntg_instance_create") } {
                Some(f) => f,
                None => return Err(format!("{} lacks ntg_instance_create", path)),
            };
        let create_call: FnCreateCall =
            match unsafe { sym(handle, "ntg_create_call") } {
                Some(f) => f,
                None => return Err(format!("{} lacks ntg_create_call", path)),
            };
        let connect: FnConnect = match unsafe { sym(handle, "ntg_connect") } {
            Some(f) => f,
            None => return Err(format!("{} lacks ntg_connect", path)),
        };
        let instance_destroy: FnInstanceDestroy =
            unsafe { sym(handle, "ntg_instance_destroy") }
                .ok_or_else(|| format!("{} lacks ntg_instance_destroy", path))?;
        let set_stream_sources: FnSetSources =
            unsafe { sym(handle, "ntg_set_stream_sources") }
                .ok_or_else(|| format!("{} lacks ntg_set_stream_sources", path))?;
        let stop: FnStop = unsafe { sym(handle, "ntg_stop") }
            .ok_or_else(|| format!("{} lacks ntg_stop", path))?;
        let pause: FnBoolOut = unsafe { sym(handle, "ntg_pause") }
            .ok_or_else(|| format!("{} lacks ntg_pause", path))?;
        let resume: FnBoolOut = unsafe { sym(handle, "ntg_resume") }
            .ok_or_else(|| format!("{} lacks ntg_resume", path))?;
        let mute: FnBoolOut = unsafe { sym(handle, "ntg_mute") }
            .ok_or_else(|| format!("{} lacks ntg_mute", path))?;
        let unmute: FnBoolOut = unsafe { sym(handle, "ntg_unmute") }
            .ok_or_else(|| format!("{} lacks ntg_unmute", path))?;
        let time: FnTime = unsafe { sym(handle, "ntg_time") }
            .ok_or_else(|| format!("{} lacks ntg_time", path))?;
        let get_version: Option<FnGetVersion> =
            unsafe { sym(handle, "ntg_get_version") };
        let last_error: Option<FnLastError> =
            unsafe { sym(handle, "ntg_last_error") };
        let string_free: Option<FnStringFree> =
            unsafe { sym(handle, "ntg_string_free") };
        let ntg = instance_create();
        if ntg.is_null() {
            return Err("ntg_instance_create() failed".to_string());
        }
        Ok(NtgLib {
            handle,
            ntg,
            lib_path: path,
            instance_create,
            instance_destroy,
            create_call,
            connect,
            set_stream_sources,
            stop,
            pause,
            resume,
            mute,
            unmute,
            time,
            get_version,
            last_error,
            string_free,
        })
    }

    pub fn lib_path(&self) -> &str {
        &self.lib_path
    }

    pub fn describe(&self) -> String {
        let mut s = format!("ntgcalls via {} (in-process FFI)", self.lib_path);
        if let Some(v) = self.version() {
            s.push(' ');
            s.push_str(&v);
        }
        s
    }

    pub fn version(&self) -> Option<String> {
        let f = self.get_version?;
        let p = f();
        if p.is_null() {
            return None;
        }
        Some(unsafe { std::ffi::CStr::from_ptr(p).to_string_lossy().to_string() })
    }

    fn error(&self, what: &str, rc: i32) -> String {
        let mut msg = format!("ntgcalls {} failed ({})", what, rc);
        if let Some(f) = self.last_error {
            let p = f();
            if !p.is_null() {
                let e = unsafe { std::ffi::CStr::from_ptr(p).to_string_lossy() };
                if !e.is_empty() {
                    msg.push_str(": ");
                    msg.push_str(&e);
                }
            }
        }
        msg
    }

    /// TinaMusic step 1: create the call, return the local SDP offer.
    /// Synchronous in 3.0.5: ntg_create_call returns the offer
    /// through the out pointer; it is freed with ntg_string_free.
    pub fn create_offer(&self, chat_id: i64) -> Result<String, String> {
        let mut buf: *mut c_char = std::ptr::null_mut();
        let rc = (self.create_call)(self.ntg, chat_id, &mut buf);
        if rc != NTG_OK {
            return Err(self.error("create_call", rc));
        }
        if buf.is_null() {
            return Err("ntg_create_call returned no params".to_string());
        }
        let s = unsafe { std::ffi::CStr::from_ptr(buf).to_string_lossy().to_string() };
        if let Some(f) = self.string_free {
            f(buf);
        }
        if s.is_empty() {
            return Err("ntg_create_call returned no params".to_string());
        }
        Ok(s)
    }

    /// TinaMusic step 5: connect the call to the remote transport
    /// payload from phone.joinGroupCall / TDLib joinVideoChat.
    pub fn connect(&self, chat_id: i64, remote_params: &str) -> Result<(), String> {
        let cparams = CString::new(remote_params)
            .map_err(|_| "remote transport contains a NUL byte".to_string())?;
        let rc = (self.connect)(self.ntg, chat_id, cparams.as_ptr(), false);
        if rc != NTG_OK {
            return Err(self.error("connect", rc));
        }
        Ok(())
    }

    /// TinaMusic step 2: attach the ffmpeg shell command(s) as sources.
    /// The media description is passed BY VALUE in 3.0.5.
    pub fn set_sources(
        &self,
        chat_id: i64,
        ffmpeg_cmd: &str,
        video_cmd: &str,
    ) -> Result<(), String> {
        let ccmd = CString::new(ffmpeg_cmd)
            .map_err(|_| "ffmpeg command contains a NUL byte".to_string())?;
        let cvcmd = CString::new(video_cmd)
            .map_err(|_| "video ffmpeg command contains a NUL byte".to_string())?;
        let audio = NtgAudioDesc {
            media_source: NTG_MEDIA_SOURCE_SHELL,
            sample_rate: 48000,
            channel_count: 2,
            input: ccmd.as_ptr() as *mut c_char,
            keep_open: false,
        };
        let video = NtgVideoDesc {
            media_source: NTG_MEDIA_SOURCE_SHELL,
            width: 1280,
            height: 720,
            fps: 30,
            input: cvcmd.as_ptr() as *mut c_char,
            keep_open: false,
        };
        let desc = NtgMediaDesc {
            microphone: &audio,
            speaker: std::ptr::null(),
            camera: if video_cmd.is_empty() {
                std::ptr::null()
            } else {
                &video
            },
            screen: std::ptr::null(),
        };
        let rc = (self.set_stream_sources)(
            self.ntg,
            chat_id,
            NTG_STREAM_MODE_CAPTURE,
            desc,
        );
        if rc != NTG_OK {
            return Err(self.error("set_stream_sources", rc));
        }
        Ok(())
    }

    fn bool_out(&self, what: &str, f: FnBoolOut, chat_id: i64) -> Result<(), String> {
        let mut ok = false;
        let rc = f(self.ntg, chat_id, &mut ok);
        if rc != NTG_OK {
            return Err(self.error(what, rc));
        }
        Ok(())
    }

    pub fn stop(&self, chat_id: i64) -> Result<(), String> {
        let rc = (self.stop)(self.ntg, chat_id);
        if rc != NTG_OK {
            return Err(self.error("stop", rc));
        }
        Ok(())
    }
    pub fn pause(&self, chat_id: i64) -> Result<(), String> {
        self.bool_out("pause", self.pause, chat_id)
    }
    pub fn resume(&self, chat_id: i64) -> Result<(), String> {
        self.bool_out("resume", self.resume, chat_id)
    }
    pub fn mute(&self, chat_id: i64) -> Result<(), String> {
        self.bool_out("mute", self.mute, chat_id)
    }
    pub fn unmute(&self, chat_id: i64) -> Result<(), String> {
        self.bool_out("unmute", self.unmute, chat_id)
    }

    pub fn time(&self, chat_id: i64) -> i64 {
        let mut t: u64 = 0;
        let rc = (self.time)(self.ntg, chat_id, NTG_STREAM_MODE_CAPTURE, &mut t);
        if rc != NTG_OK {
            -1
        } else {
            t as i64
        }
    }
}

impl Drop for NtgLib {
    fn drop(&mut self) {
        if !self.ntg.is_null() {
            let _ = (self.instance_destroy)(self.ntg);
            self.ntg = std::ptr::null_mut();
        }
        if !self.handle.is_null() {
            unsafe { dlclose(self.handle) };
            self.handle = std::ptr::null_mut();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn load_with_bogus_explicit_path_falls_back_or_fails_honestly() {
        // load() tries the explicit path first, then every candidate.
        // A bogus explicit path therefore either falls back to a REAL
        // built libntgcalls.so (version 3.0.5, real describe) or fails
        // with the exact dlopen reason — never a panic, never a fake
        // library. Before the C build landed this test failed the
        // load; now it proves the fallback.
        match NtgLib::load(Some("/nonexistent/libntgcalls.so")) {
            Ok(lib) => {
                eprintln!("ntg loaded via fallback: {}", lib.describe());
                assert!(lib.lib_path().ends_with("libntgcalls.so"));
                assert_eq!(lib.version().as_deref(), Some("3.0.5"));
            }
            Err(e) => {
                eprintln!("ntg load error (expected when unbuilt): {}", e);
                assert!(
                    e.contains("dlopen") || e.contains("not found"),
                    "error names the failure: {}",
                    e
                );
            }
        }
    }

    #[test]
    fn candidates_name_the_real_paths() {
        let c = NtgLib::candidates();
        assert!(c.iter().any(|p| p.contains("shared-output")));
        assert!(c.iter().any(|p| p == "libntgcalls.so"));
    }

    #[test]
    fn create_offer_via_built_lib() {
        // Phase 5: when the C build landed (refs/ntgcalls/
        // shared-output/lib/libntgcalls.so), the in-process FFI must
        // load it and ntg_create_call must return a real local SDP
        // offer. Skipped when the .so is absent.
        let Some(lib) = NtgLib::first_existing() else {
            eprintln!("SKIP create_offer_via_built_lib: no libntgcalls.so built");
            return;
        };
        let ntg = NtgLib::load(Some(&lib)).expect("built .so must load");
        eprintln!("ntg describe: {}", ntg.describe());
        assert_eq!(ntg.version().as_deref(), Some("3.0.5"));
        let offer = ntg.create_offer(-1004244).expect("create_offer must succeed");
        eprintln!("offer len {} head: {}", offer.len(), &offer[..offer.len().min(120)]);
        assert!(offer.len() > 100, "offer len {}", offer.len());
        assert!(offer.contains("ufrag"), "offer carries an ICE ufrag");
        let _ = ntg.stop(-1004244);
    }
}
