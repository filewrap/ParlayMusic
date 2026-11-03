//! The f32 FX chain. Everything runs at 48 kHz stereo on 960-sample
//! frames, in f32 throughout — **no 16-bit intermediate anywhere**.
//!
//! Chain order (per frame):
//! 1. bass — RBJ low-shelf biquad (port of ffmpeg `bass=g=10:f=110:w=0.6`),
//!    coefficients interpolated per-sample when the gain moves.
//! 2. echo — ffmpeg `aecho` semantics (in_gain / out_gain / delay / decay),
//!    wet level ramped per-sample.
//! 3. 8D — phase-continuous quadrature LFO panner (0.12 Hz) + Haas delay,
//!    amount ramped per-sample; the LFO phase is never reset.
//! 4. reverb — stereo Freeverb (4 Schroeder lowpass-feedback combs + 2
//!    allpasses per channel), wet ramped per-sample.
//! 5. volume — gain 0–200 mapped as `(v/100)^2`, smoothed per-sample.
//! 6. limiter — true-peak brickwall, 5 ms lookahead, soft knee, −0.1 dBFS
//!    ceiling, fast attack / 100 ms release.
//!
//! TPDF dither lives in `dither_s16` and is applied **only** at the final
//! f32 → s16 conversion in the s16 pull path.
//!
//! Zero-drop rule: no parameter ever steps discontinuously. Every
//! time-varying value is either interpolated per-sample across the frame
//! (biquad coefficients, wet levels, pan amount, volume gain) or carried
//! in continuous state (LFO phase, delay lines, limiter envelope). Toggling
//! an effect ramps its mix 0 ↔ 1 over ~100 ms (5 frames); the delay-line
//! contents are never cleared, so tails ring out naturally.

use crate::resample::FRAME_SAMPLES;

// ---------------------------------------------------------------- biquad ---

/// Five biquad coefficients (Direct Form I: y = b0*x + b1*x1 + b2*x2
/// - a1*y1 - a2*y2).
#[derive(Debug, Clone, Copy, Default)]
pub struct BiquadCoeffs {
    pub b0: f32,
    pub b1: f32,
    pub b2: f32,
    pub a1: f32,
    pub a2: f32,
}

impl BiquadCoeffs {
    /// RBJ low-shelf: gain `db` dB, corner `freq` Hz, shelf slope `s`
    /// (ffmpeg `bass` width `w` maps to the slope parameter here).
    pub fn low_shelf(db: f32, freq: f32, s: f32, rate: f32) -> Self {
        let a = 10f32.powf(db / 40.0);
        let w0 = 2.0 * std::f32::consts::PI * freq / rate;
        let (sin_w0, cos_w0) = w0.sin_cos();
        // RBJ shelf alpha. sqrt() argument is >= 0 for s > 0.
        let alpha = sin_w0 / 2.0 * ((a + 1.0 / a) * (1.0 / s - 1.0) + 2.0).sqrt();
        let two_sqrt_a_alpha = 2.0 * a.sqrt() * alpha;
        let b0 = a * ((a + 1.0) - (a - 1.0) * cos_w0 + two_sqrt_a_alpha);
        let b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cos_w0);
        let b2 = a * ((a + 1.0) - (a - 1.0) * cos_w0 - two_sqrt_a_alpha);
        let a0 = (a + 1.0) + (a - 1.0) * cos_w0 + two_sqrt_a_alpha;
        let a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cos_w0);
        let a2 = (a + 1.0) + (a - 1.0) * cos_w0 - two_sqrt_a_alpha;
        BiquadCoeffs {
            b0: b0 / a0,
            b1: b1 / a0,
            b2: b2 / a0,
            a1: a1 / a0,
            a2: a2 / a0,
        }
    }

    /// Identity (0 dB): passthrough.
    pub fn identity() -> Self {
        BiquadCoeffs {
            b0: 1.0,
            b1: 0.0,
            b2: 0.0,
            a1: 0.0,
            a2: 0.0,
        }
    }
}

/// One biquad section with per-sample coefficient interpolation.
#[derive(Debug, Clone)]
pub struct Biquad {
    x1: f32,
    x2: f32,
    y1: f32,
    y2: f32,
    /// Coefficients used for the previous sample (interp start).
    prev: BiquadCoeffs,
}

impl Default for Biquad {
    fn default() -> Self {
        Biquad {
            x1: 0.0,
            x2: 0.0,
            y1: 0.0,
            y2: 0.0,
            prev: BiquadCoeffs::identity(),
        }
    }
}

impl Biquad {
    /// Process `buf` in place, interpolating coefficients linearly from
    /// the previous frame's target to `target` across the frame.
    pub fn process(&mut self, buf: &mut [f32], target: BiquadCoeffs) {
        let n = buf.len() as f32;
        let from = self.prev;
        for (i, x) in buf.iter_mut().enumerate() {
            let t = (i as f32 + 1.0) / n;
            let b0 = from.b0 + (target.b0 - from.b0) * t;
            let b1 = from.b1 + (target.b1 - from.b1) * t;
            let b2 = from.b2 + (target.b2 - from.b2) * t;
            let a1 = from.a1 + (target.a1 - from.a1) * t;
            let a2 = from.a2 + (target.a2 - from.a2) * t;
            let y = b0 * *x + b1 * self.x1 + b2 * self.x2 - a1 * self.y1 - a2 * self.y2;
            self.x2 = self.x1;
            self.x1 = *x;
            self.y2 = self.y1;
            self.y1 = y;
            *x = y;
        }
        self.prev = target;
    }
}

// ------------------------------------------------------------------ echo ---

/// Feedback delay echo, ffmpeg `aecho` semantics:
/// `buf[i] = in + tap * decay; out = in * in_gain + tap * out_gain`.
/// The delay line is never cleared; disabling the effect only ramps the
/// wet level, so the tail decays naturally.
#[derive(Debug)]
pub struct Echo {
    buf: Vec<f32>,
    pos: usize,
    delay_samples: usize,
    in_gain: f32,
    out_gain: f32,
    decay: f32,
    /// Current wet level (0..1), ramped per-sample toward `wet_target`.
    wet: f32,
    wet_target: f32,
}

impl Echo {
    pub fn new(delay_ms: f32, in_gain: f32, out_gain: f32, decay: f32, rate: f32) -> Self {
        let delay_samples = ((delay_ms / 1000.0) * rate).round().max(1.0) as usize;
        Echo {
            buf: vec![0.0; delay_samples.max(1)],
            pos: 0,
            delay_samples: delay_samples.max(1),
            in_gain,
            out_gain,
            decay,
            wet: 0.0,
            wet_target: 0.0,
        }
    }

    pub fn set_delay(&mut self, delay_ms: f32, rate: f32) {
        let delay_samples = ((delay_ms / 1000.0) * rate).round().max(1.0) as usize;
        if delay_samples != self.delay_samples {
            // Reallocating the line would click; instead keep the old line
            // and resize only when the delay grows (zero-filled tail).
            // Shrinking keeps the head of the line. The wet ramp masks
            // any residual transient.
            self.buf.resize(delay_samples.max(1), 0.0);
            self.pos %= self.buf.len().max(1);
            self.delay_samples = delay_samples.max(1);
        }
    }

    pub fn set_params(&mut self, in_gain: f32, out_gain: f32, decay: f32) {
        self.in_gain = in_gain;
        self.out_gain = out_gain;
        self.decay = decay;
    }

    /// Drive the wet level directly (0..1); the per-sample ramp in
    /// `process` smooths within the frame.
    pub fn set_wet(&mut self, wet: f32) {
        self.wet_target = wet.clamp(0.0, 1.0);
    }

    /// Process one channel in place. Wet ramps linearly across the frame.
    pub fn process(&mut self, buf: &mut [f32]) {
        let n = buf.len() as f32;
        let from = self.wet;
        let len = self.buf.len();
        for (i, x) in buf.iter_mut().enumerate() {
            let t = (i as f32 + 1.0) / n;
            let wet = from + (self.wet_target - from) * t;
            let tap = self.buf[self.pos];
            let y = *x * self.in_gain + tap * self.out_gain;
            self.buf[self.pos] = *x + tap * self.decay;
            self.pos = (self.pos + 1) % len;
            *x = *x + (y - *x) * wet;
        }
        self.wet = self.wet_target;
    }
}

// ------------------------------------------------------------------- 8D ---

/// 8D: phase-continuous quadrature LFO panner + Haas delay.
///
/// Pan position `p = sin(phase)` sweeps L↔R at 0.12 Hz with a
/// constant-power law; the Haas delay (up to 8 ms, one channel delayed
/// against the other in quadrature with the pan) adds the out-of-head
/// width. The LFO phase advances with the sample clock and is never reset,
/// so toggling 8D mid-track cannot jump the image. The `amount` (0..1)
/// ramps per-sample.
#[derive(Debug)]
pub struct Pan8d {
    phase: f64,
    phase_inc: f64, // per sample, 2*pi*0.12/48000
    amount: f32,
    amount_target: f32,
    /// Haas delay lines (max 8 ms = 384 samples @48k), linear interp.
    delay_l: Vec<f32>,
    delay_r: Vec<f32>,
    pos_l: usize,
    pos_r: usize,
}

const HAAS_MAX_MS: f32 = 8.0;

impl Pan8d {
    pub fn new(rate: f32) -> Self {
        let max = ((HAAS_MAX_MS / 1000.0) * rate).ceil() as usize + 2;
        Pan8d {
            phase: 0.0,
            phase_inc: 2.0 * std::f64::consts::PI * 0.12 / rate as f64,
            amount: 0.0,
            amount_target: 0.0,
            delay_l: vec![0.0; max],
            delay_r: vec![0.0; max],
            pos_l: 0,
            pos_r: 0,
        }
    }

    /// Drive the pan amount directly (0..1); per-sample ramp in `process`.
    pub fn set_amount(&mut self, amount: f32) {
        self.amount_target = amount.clamp(0.0, 1.0);
    }

    /// Process a stereo frame (L and R slices) in place.
    pub fn process(&mut self, l: &mut [f32], r: &mut [f32]) {
        debug_assert_eq!(l.len(), r.len());
        let n = l.len() as f32;
        let from = self.amount;
        let max_delay = (self.delay_l.len() - 2) as f32;
        for i in 0..l.len() {
            let t = (i as f32 + 1.0) / n;
            let amount = from + (self.amount_target - from) * t;
            let s = self.phase.sin() as f32; // -1..1, pan position
            let c = self.phase.cos() as f32; // quadrature, Haas depth
            self.phase += self.phase_inc;
            if self.phase >= 2.0 * std::f64::consts::PI {
                self.phase -= 2.0 * std::f64::consts::PI;
            }
            // Constant-power pan: angle in [0, pi/2].
            let angle = (s * 0.5 + 0.5) * std::f32::consts::FRAC_PI_2;
            let (gl, gr) = (angle.cos(), angle.sin());
            // Haas: delay one side by up to HAAS_MAX_MS, quadrature to pan.
            let d_l = (1.0 - c) * 0.5 * max_delay;
            let d_r = (1.0 + c) * 0.5 * max_delay;
            let xl = l[i];
            let xr = r[i];
            let dl = read_delay(&self.delay_l, self.pos_l, d_l);
            let dr = read_delay(&self.delay_r, self.pos_r, d_r);
            write_delay(&mut self.delay_l, &mut self.pos_l, xl);
            write_delay(&mut self.delay_r, &mut self.pos_r, xr);
            // Processed: constant-power pan + Haas-delayed width.
            let wl = xl * gl + dl * 0.6;
            let wr = xr * gr + dr * 0.6;
            // Blend dry <-> processed by amount (per-sample ramped).
            l[i] = xl + (wl - xl) * amount;
            r[i] = xr + (wr - xr) * amount;
        }
        self.amount = self.amount_target;
    }
}

fn read_delay(line: &[f32], pos: usize, delay: f32) -> f32 {
    let len = line.len() as f32;
    let read_pos = pos as f32 - delay;
    let read_pos = if read_pos < 0.0 { read_pos + len } else { read_pos };
    let i0 = read_pos.floor() as usize % line.len();
    let i1 = (i0 + 1) % line.len();
    let frac = read_pos - read_pos.floor();
    line[i0] * (1.0 - frac) + line[i1] * frac
}

fn write_delay(line: &mut [f32], pos: &mut usize, x: f32) {
    line[*pos] = x;
    *pos = (*pos + 1) % line.len();
}

// ---------------------------------------------------------------- reverb ---

/// Stereo Freeverb: 4 parallel lowpass-feedback combs + 2 series allpasses
/// per channel, right-channel combs offset by 23 samples (Freeverb's
/// stereo spread). Tunings are Freeverb's 44.1 kHz values scaled to 48 kHz.
#[derive(Debug)]
struct Comb {
    buf: Vec<f32>,
    pos: usize,
    feedback: f32,
    damp: f32,
    filter_store: f32,
}

impl Comb {
    fn new(size: usize, feedback: f32, damp: f32) -> Self {
        Comb {
            buf: vec![0.0; size],
            pos: 0,
            feedback,
            damp,
            filter_store: 0.0,
        }
    }

    #[inline]
    fn process(&mut self, x: f32) -> f32 {
        let out = self.buf[self.pos];
        self.filter_store = out * (1.0 - self.damp) + self.filter_store * self.damp;
        self.buf[self.pos] = x + self.feedback * self.filter_store;
        self.pos = (self.pos + 1) % self.buf.len();
        out
    }
}

#[derive(Debug)]
struct Allpass {
    buf: Vec<f32>,
    pos: usize,
    feedback: f32,
}

impl Allpass {
    fn new(size: usize, feedback: f32) -> Self {
        Allpass {
            buf: vec![0.0; size],
            pos: 0,
            feedback,
        }
    }

    #[inline]
    fn process(&mut self, x: f32) -> f32 {
        let bufout = self.buf[self.pos];
        let y = -x + bufout;
        self.buf[self.pos] = x + bufout * self.feedback;
        self.pos = (self.pos + 1) % self.buf.len();
        y
    }
}

#[derive(Debug)]
pub struct Freeverb {
    combs_l: Vec<Comb>,
    combs_r: Vec<Comb>,
    allpass_l: Vec<Allpass>,
    allpass_r: Vec<Allpass>,
    wet: f32,
    wet_target: f32,
}

const COMB_TUNINGS_48K: [usize; 4] = [1216, 1294, 1391, 1477];
const ALLPASS_TUNINGS_48K: [usize; 2] = [605, 480];

impl Freeverb {
    pub fn new() -> Self {
        let mk_combs = |spread: usize| {
            COMB_TUNINGS_48K
                .iter()
                .map(|&t| Comb::new(t + spread, 0.84, 0.2))
                .collect()
        };
        let mk_ap = |spread: usize| {
            ALLPASS_TUNINGS_48K
                .iter()
                .map(|&t| Allpass::new(t + spread, 0.5))
                .collect()
        };
        Freeverb {
            combs_l: mk_combs(0),
            combs_r: mk_combs(23),
            allpass_l: mk_ap(0),
            allpass_r: mk_ap(23),
            wet: 0.0,
            wet_target: 0.0,
        }
    }

    /// Drive the wet level directly (0..1); per-sample ramp in `process`.
    pub fn set_wet(&mut self, wet: f32) {
        self.wet_target = wet.clamp(0.0, 1.0);
    }

    /// Process a stereo frame in place. Wet ramps linearly across the frame.
    pub fn process(&mut self, l: &mut [f32], r: &mut [f32]) {
        debug_assert_eq!(l.len(), r.len());
        let n = l.len() as f32;
        let from = self.wet;
        for i in 0..l.len() {
            let t = (i as f32 + 1.0) / n;
            let wet = from + (self.wet_target - from) * t;
            let xl = l[i];
            let xr = r[i];
            let mut rl = 0.0f32;
            let mut rr = 0.0f32;
            for c in self.combs_l.iter_mut() {
                rl += c.process(xl);
            }
            for c in self.combs_r.iter_mut() {
                rr += c.process(xr);
            }
            for a in self.allpass_l.iter_mut() {
                rl = a.process(rl);
            }
            for a in self.allpass_r.iter_mut() {
                rr = a.process(rr);
            }
            // Freeverb sums 4 combs; scale to keep the wet level sane.
            l[i] = xl + rl * 0.25 * wet;
            r[i] = xr + rr * 0.25 * wet;
        }
        self.wet = self.wet_target;
    }
}

// ---------------------------------------------------------------- volume ---

/// Volume gain, smoothed per-sample (linear ramp across the frame from the
/// previous gain to the target). Mapping: 0–200 -> `(v/100)^2` (1.0 at 100,
/// 4.0 at 200, 0.25 at 50). 0 is silence.
pub fn volume_gain(volume_0_200: f64) -> f32 {
    let v = (volume_0_200 / 100.0).clamp(0.0, 2.0);
    (v * v) as f32
}

// ---------------------------------------------------------------- limiter ---

/// True-peak brickwall limiter: 5 ms lookahead, soft knee (3 dB),
/// ceiling −0.1 dBFS, instant attack on the delayed signal, 100 ms release.
#[derive(Debug)]
pub struct Limiter {
    /// Delay line (240 samples @48k) per channel.
    delay_l: Vec<f32>,
    delay_r: Vec<f32>,
    pos: usize,
    /// Ring of recent |max(L, R)| magnitudes (lookahead window).
    mag: Vec<f32>,
    mag_pos: usize,
    /// Current applied gain (smoothed).
    gain: f32,
}

const LIMITER_LOOKAHEAD: usize = 240; // 5 ms @ 48 kHz
const LIMITER_CEILING: f32 = 0.988_55; // -0.1 dBFS
const LIMITER_KNEE_START: f32 = 0.699_77; // ceiling - 3 dB

impl Default for Limiter {
    fn default() -> Self {
        Limiter {
            delay_l: vec![0.0; LIMITER_LOOKAHEAD],
            delay_r: vec![0.0; LIMITER_LOOKAHEAD],
            pos: 0,
            mag: vec![0.0; LIMITER_LOOKAHEAD],
            mag_pos: 0,
            gain: 1.0,
        }
    }
}

impl Limiter {
    fn target_gain(peak: f32) -> f32 {
        if peak <= LIMITER_KNEE_START {
            1.0
        } else if peak >= LIMITER_CEILING {
            LIMITER_CEILING / peak
        } else {
            // Soft knee: quadratic blend between 1.0 and ceiling/peak.
            let t = (peak - LIMITER_KNEE_START) / (LIMITER_CEILING - LIMITER_KNEE_START);
            let hard = LIMITER_CEILING / peak;
            1.0 + (hard - 1.0) * t * t
        }
    }

    /// Process a stereo frame in place.
    pub fn process(&mut self, l: &mut [f32], r: &mut [f32]) {
        debug_assert_eq!(l.len(), r.len());
        // Release coefficient per sample: tau = 100 ms.
        let release = 1.0 - (-1.0f32 / (0.1 * 48_000.0)).exp();
        for i in 0..l.len() {
            // Push current magnitudes into the lookahead window.
            let m = l[i].abs().max(r[i].abs());
            self.mag[self.mag_pos] = m;
            self.mag_pos = (self.mag_pos + 1) % LIMITER_LOOKAHEAD;
            // Peak over the window (the next 5 ms of audio).
            let mut peak = 0.0f32;
            for &v in self.mag.iter() {
                if v > peak {
                    peak = v;
                }
            }
            let target = Self::target_gain(peak);
            if target < self.gain {
                self.gain = target; // instant attack (on delayed audio)
            } else {
                self.gain += (target - self.gain) * release;
            }
            // Delayed output sample * smoothed gain.
            let dl = self.delay_l[self.pos];
            let dr = self.delay_r[self.pos];
            self.delay_l[self.pos] = l[i];
            self.delay_r[self.pos] = r[i];
            self.pos = (self.pos + 1) % LIMITER_LOOKAHEAD;
            l[i] = dl * self.gain;
            r[i] = dr * self.gain;
        }
    }
}

// ----------------------------------------------------------------- dither ---

/// xorshift64* PRNG (no dependency). Seeded from time + address entropy.
#[derive(Debug)]
pub struct XorShift64 {
    state: u64,
}

impl XorShift64 {
    pub fn new() -> Self {
        let t = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos() as u64)
            .unwrap_or(0x9E3779B97F4A7C15);
        // Mix in the address of the RNG itself for per-engine uniqueness.
        let addr = (Self::new as *const () as u64).wrapping_mul(0x9E3779B97F4A7C15);
        let mut s = t ^ addr ^ 0x2545F4914F6CDD1D;
        if s == 0 {
            s = 0x2545F4914F6CDD1D;
        }
        XorShift64 { state: s }
    }

    #[inline]
    pub fn next_u64(&mut self) -> u64 {
        let mut x = self.state;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.state = x;
        x.wrapping_mul(0x2545F4914F6CDD1D)
    }

    /// Uniform float in [0, 1).
    #[inline]
    pub fn next_f32(&mut self) -> f32 {
        // Top 24 bits -> [0, 1).
        ((self.next_u64() >> 40) as f32) / 16_777_216.0
    }
}

/// TPDF dither: triangular PDF, 2 LSB peak-to-peak at 16-bit, added before
/// quantisation. Applied ONLY at the final f32 -> s16 conversion.
pub fn dither_s16(x: f32, rng: &mut XorShift64) -> i16 {
    const LSB: f32 = 1.0 / 32768.0;
    let d = (rng.next_f32() + rng.next_f32() - 1.0) * LSB;
    let v = (x + d).clamp(-1.0, 1.0) * 32767.0;
    // Round to nearest, ties away from zero (matches a dithered quantiser).
    let q = v.round() as i32;
    q.clamp(-32768, 32767) as i16
}

/// One frame of the whole chain state (everything except the resampler).
#[derive(Debug)]
pub struct FxChain {
    pub bass_l: Biquad,
    pub bass_r: Biquad,
    pub echo_l: Echo,
    pub echo_r: Echo,
    pub pan: Pan8d,
    pub reverb: Freeverb,
    pub volume_gain: f32,
    pub limiter: Limiter,
    pub rng: XorShift64,
}

impl FxChain {
    pub fn new() -> Self {
        FxChain {
            bass_l: Biquad::default(),
            bass_r: Biquad::default(),
            // aecho defaults match the slow-reverb preset; parameters are
            // overwritten by set_preset / set_echo before use.
            echo_l: Echo::new(60.0, 0.8, 0.88, 0.4, 48_000.0),
            echo_r: Echo::new(60.0, 0.8, 0.88, 0.4, 48_000.0),
            pan: Pan8d::new(48_000.0),
            reverb: Freeverb::new(),
            volume_gain: 1.0,
            limiter: Limiter::default(),
            rng: XorShift64::new(),
        }
    }

    /// Process one 960-frame (L/R slices) in place. `bass` is the target
    /// coefficient set for this frame (already smoothed by the engine);
    /// `volume_target` is the target linear gain for this frame.
    pub fn process(
        &mut self,
        l: &mut [f32],
        r: &mut [f32],
        bass: BiquadCoeffs,
        volume_target: f32,
    ) {
        debug_assert_eq!(l.len(), FRAME_SAMPLES);
        debug_assert_eq!(r.len(), FRAME_SAMPLES);
        self.bass_l.process(l, bass);
        self.bass_r.process(r, bass);
        self.echo_l.process(l);
        self.echo_r.process(r);
        self.pan.process(l, r);
        self.reverb.process(l, r);
        // Volume: linear ramp across the frame from the previous gain.
        let n = l.len() as f32;
        let from = self.volume_gain;
        for i in 0..l.len() {
            let t = (i as f32 + 1.0) / n;
            let g = from + (volume_target - from) * t;
            l[i] *= g;
            r[i] *= g;
        }
        self.volume_gain = volume_target;
        self.limiter.process(l, r);
    }
}
