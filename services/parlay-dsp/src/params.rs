//! FX parameters: targets set from the C-ABI, smoothed currents advanced
//! once per pulled frame, and the five named presets ported from
//! `services/audio-fx-rust` (which itself ports the reference bot's ffmpeg
//! filter chains).
//!
//! Preset mapping (ffmpeg chain -> parlay-dsp parameters):
//! * `original`    — everything off, speed 1.0.
//! * `nightcore`   — speed 1.25. (Deviation, documented in README.md: the
//!   ffmpeg chain was `asetrate=44100*1.25,atempo=1/1.25`, i.e. pitch up
//!   with tempo restored; parlay-dsp implements nightcore as a pure
//!   resample-ratio speed-up — pitch AND tempo up, vinyl-style — per the
//!   locked doc's "dynamic resampler ratio" design.)
//! * `bass-boost`  — bass +10 dB @ 110 Hz, shelf slope 0.6, post-gain 0.9
//!   (ffmpeg `bass=g=10:f=110:w=0.6,volume=0.9`).
//! * `slow-reverb` — speed 0.8 + echo (60 ms, decay 0.4) + Freeverb
//!   (ffmpeg `atempo=0.8,aecho=0.8:0.88:60:0.4`; the locked doc upgrades
//!   the tail to a stereo Freeverb/FDN reverb — the aecho is kept as the
//!   early echo and the Freeverb supplies the tail).
//! * `8d`          — echo (40 ms, decay 0.3) + LFO pan 0.12 Hz + Haas
//!   (ffmpeg `apulsator=mode=sine:hz=0.12,aecho=0.8:0.88:40:0.3`).
//!
//! Smoothing (zero-drop): per pulled frame the engine steps each current
//! toward its target by at most a fixed increment — speed 0.05/frame,
//! bass 1 dB/frame, echo/reverb/8d wet 0.2/frame (full on/off in 5 frames
//! = 100 ms). The chain itself interpolates per-sample inside the frame
//! (see fx.rs), so even a one-frame step cannot click.

use crate::fx::BiquadCoeffs;

pub const SPEED_MIN: f64 = 0.5;
pub const SPEED_MAX: f64 = 2.0;
pub const BASS_MIN_DB: f64 = -24.0;
pub const BASS_MAX_DB: f64 = 24.0;
pub const VOLUME_MIN: f64 = 0.0;
pub const VOLUME_MAX: f64 = 200.0;

/// Bass shelf constants from the port (ffmpeg `bass=g=10:f=110:w=0.6`).
pub const BASS_FREQ_HZ: f32 = 110.0;
pub const BASS_SLOPE: f32 = 0.6;
/// Post-bass compensation from the port (`volume=0.9` in the bass chain).
pub const BASS_POST_GAIN: f32 = 0.9;

/// Effect toggles + continuous parameters (the "target" side).
#[derive(Debug, Clone)]
pub struct FxTargets {
    pub speed: f64,
    pub bass_db: f64,
    pub volume: f64, // 0..200
    pub echo_on: bool,
    pub echo_delay_ms: f64,
    pub echo_decay: f64,
    pub reverb_on: bool,
    pub reverb_wet: f64,
    pub pan8d_on: bool,
}

impl Default for FxTargets {
    fn default() -> Self {
        FxTargets {
            speed: 1.0,
            bass_db: 0.0,
            volume: 100.0,
            echo_on: false,
            echo_delay_ms: 60.0,
            echo_decay: 0.4,
            reverb_on: false,
            reverb_wet: 0.35,
            pan8d_on: false,
        }
    }
}

/// Named preset -> targets. Unknown names are rejected by the caller.
pub fn preset(name: &str) -> Option<FxTargets> {
    let mut t = FxTargets::default();
    match name {
        "original" => Some(t),
        "nightcore" => {
            t.speed = 1.25;
            Some(t)
        }
        "bass-boost" => {
            t.bass_db = 10.0;
            Some(t)
        }
        "slow-reverb" => {
            t.speed = 0.8;
            t.echo_on = true;
            t.echo_delay_ms = 60.0;
            t.echo_decay = 0.4;
            t.reverb_on = true;
            t.reverb_wet = 0.35;
            Some(t)
        }
        "8d" => {
            t.echo_on = true;
            t.echo_delay_ms = 40.0;
            t.echo_decay = 0.3;
            t.pan8d_on = true;
            Some(t)
        }
        _ => None,
    }
}

/// Smoothed currents, advanced once per frame by `step_toward`.
#[derive(Debug, Clone)]
pub struct FxCurrents {
    pub speed: f64,
    pub bass_db: f64,
    /// Wet levels 0..1 for echo / reverb / 8D amount.
    pub echo_wet: f32,
    pub reverb_wet: f32,
    pub pan_amount: f32,
}

impl Default for FxCurrents {
    fn default() -> Self {
        FxCurrents {
            speed: 1.0,
            bass_db: 0.0,
            echo_wet: 0.0,
            reverb_wet: 0.0,
            pan_amount: 0.0,
        }
    }
}

fn step_toward(current: f64, target: f64, max_step: f64) -> f64 {
    let d = target - current;
    if d.abs() <= max_step {
        target
    } else {
        current + max_step * d.signum()
    }
}

impl FxCurrents {
    /// Advance currents toward targets by at most one frame-step each.
    pub fn step(&mut self, t: &FxTargets) {
        self.speed = step_toward(self.speed, t.speed, 0.05);
        self.bass_db = step_toward(self.bass_db, t.bass_db, 1.0);
        let echo_target = if t.echo_on { 1.0f32 } else { 0.0 };
        let reverb_target = if t.reverb_on { t.reverb_wet as f32 } else { 0.0 };
        let pan_target = if t.pan8d_on { 1.0f32 } else { 0.0 };
        self.echo_wet = step_toward(self.echo_wet as f64, echo_target as f64, 0.2) as f32;
        self.reverb_wet = step_toward(self.reverb_wet as f64, reverb_target as f64, 0.2) as f32;
        self.pan_amount = step_toward(self.pan_amount as f64, pan_target as f64, 0.2) as f32;
    }

    /// Bass coefficients for the current smoothed gain, including the
    /// 0.9 post-gain compensation blended in with the boost.
    pub fn bass_coeffs(&self) -> (BiquadCoeffs, f32) {
        let coeffs = BiquadCoeffs::low_shelf(self.bass_db as f32, BASS_FREQ_HZ, BASS_SLOPE, 48_000.0);
        // Post-gain 0.9 only while bass is boosted (ramps with the gain).
        let post = if self.bass_db > 0.0 {
            1.0 - (1.0 - BASS_POST_GAIN) * (self.bass_db as f32 / 10.0).clamp(0.0, 1.0)
        } else {
            1.0
        };
        (coeffs, post)
    }
}

/// Loop modes (C-ABI `parlay_dsp_set_loop`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LoopMode {
    Off = 0,
    Track = 1,
}

impl LoopMode {
    pub fn from_i32(v: i32) -> Option<Self> {
        match v {
            0 => Some(LoopMode::Off),
            1 => Some(LoopMode::Track),
            _ => None,
        }
    }
}
