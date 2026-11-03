//! Engine: two decode slots, one resampler, one FX chain, one 960-frame
//! output at a time.
//!
//! Concurrency model:
//! * The engine is shared as `Arc<Engine>`; all mutable state lives behind
//!   one `Mutex<EngineInner>`.
//! * `push` never takes that mutex — it clones the slot's `Arc<SharedBuf>`
//!   (lock-free) and appends under the buffer's own lock. A pull that is
//!   blocked inside a decoder read (waiting for bytes) therefore can never
//!   starve a push: the classic push/pull deadlock is structurally
//!   impossible.
//! * `destroy` sets the shutdown flag on both buffers, waking any blocked
//!   decoder read with EOF.
//!
//! Gapless flow: the resampler consumes from a staging queue that is
//! filled from the active slot; when the active slot drains mid-chunk, the
//! next slot's samples continue filling the *same* chunk, so one 960-frame
//! output can straddle a track boundary with zero inserted silence. The
//! resampler is never reset at a boundary (only its ratio changes, ramped,
//! when the input rate changes).

use crate::fx::{volume_gain, FxChain};
use crate::params::{preset, FxCurrents, FxTargets, LoopMode, BASS_MAX_DB, BASS_MIN_DB,
                    SPEED_MAX, SPEED_MIN, VOLUME_MAX, VOLUME_MIN};
use crate::resample::{Resampler48k, FRAME_SAMPLES};
use crate::slot::{DecodeSlot, SlotOutcome};
use crate::source::SharedBuf;
use std::sync::{Arc, Mutex};

pub const PULL_OK: i32 = 0;
pub const PULL_NEED_INPUT: i32 = 1;
pub const PULL_EOS: i32 = 2;

/// Engine states (also the C-ABI `state` field in stats).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum State {
    Idle = 0,
    Playing = 1,
    NeedInput = 2,
    Drained = 3,
    Error = 4,
}

/// What a pull produced.
pub enum FrameOut {
    /// A 960-frame is in the engine's scratch output buffers.
    Frame { transition: i32 },
    NeedInput,
    Eos,
}

/// C-ABI-visible stats (mirrored in `parlay_dsp.h`).
#[derive(Debug, Clone, Copy, Default)]
pub struct Stats {
    pub state: i32,
    pub active_slot: i32,
    pub free_slot: i32,
    pub loop_mode: i32,
    pub frames_pulled: u64,
    pub samples_pulled: u64,
    pub transitions: u64,
    pub loops: u64,
    pub underruns: u64,
    pub speed: f64,
    pub bass_db: f64,
    pub volume: f64,
    pub input_sample_rate: i32,
    pub input_channels: i32,
}

struct EngineInner {
    slots: [DecodeSlot; 2],
    active: Option<usize>,
    resampler: Option<Resampler48k>,
    fx: FxChain,
    targets: FxTargets,
    currents: FxCurrents,
    loop_mode: LoopMode,
    state: State,
    last_error: String,
    frames_pulled: u64,
    samples_pulled: u64,
    transitions: u64,
    loops: u64,
    underruns: u64,
    /// Transition (1 = track boundary, 2 = loop restart) recorded while
    /// filling a frame; reported with the frame that carries the boundary.
    pending_transition: i32,
    /// End-of-stream flush: draining the resampler's tail.
    flushing: bool,
    flush_calls: usize,
    /// Staging queue feeding the resampler (per-channel, input-rate).
    staging_l: Vec<f32>,
    staging_r: Vec<f32>,
    /// Scratch 960-frame output (per-channel, 48 kHz).
    out_l: Vec<f32>,
    out_r: Vec<f32>,
}

pub struct Engine {
    slot_bufs: [Arc<SharedBuf>; 2],
    inner: Mutex<EngineInner>,
}

impl Engine {
    pub fn new() -> Arc<Self> {
        let slot_bufs = [SharedBuf::new(), SharedBuf::new()];
        let inner = EngineInner {
            slots: [
                DecodeSlot::new(Arc::clone(&slot_bufs[0])),
                DecodeSlot::new(Arc::clone(&slot_bufs[1])),
            ],
            active: None,
            resampler: None,
            fx: FxChain::new(),
            targets: FxTargets::default(),
            currents: FxCurrents::default(),
            loop_mode: LoopMode::Off,
            state: State::Idle,
            last_error: String::new(),
            frames_pulled: 0,
            samples_pulled: 0,
            transitions: 0,
            loops: 0,
            underruns: 0,
            pending_transition: 0,
            flushing: false,
            flush_calls: 0,
            staging_l: Vec::new(),
            staging_r: Vec::new(),
            out_l: vec![0.0; FRAME_SAMPLES],
            out_r: vec![0.0; FRAME_SAMPLES],
        };
        Arc::new(Engine {
            slot_bufs,
            inner: Mutex::new(inner),
        })
    }

    /// Wake blocked readers (called from destroy).
    pub fn shutdown(&self) {
        for b in &self.slot_bufs {
            b.shutdown();
        }
    }

    // ------------------------------------------------------------ input ---

    fn check_slot(slot: i32) -> Result<usize, String> {
        if !(0..=1).contains(&slot) {
            return Err(format!("invalid slot {slot} (must be 0 or 1)"));
        }
        Ok(slot as usize)
    }

    /// Append encoded bytes to a slot. Never takes the engine mutex.
    pub fn push(&self, slot: i32, data: &[u8]) -> Result<(), String> {
        let slot = Self::check_slot(slot)?;
        self.slot_bufs[slot].push(data);
        Ok(())
    }

    /// Mark a slot's input complete.
    pub fn finish_input(&self, slot: i32) -> Result<(), String> {
        let slot = Self::check_slot(slot)?;
        self.slot_bufs[slot].finish();
        Ok(())
    }

    /// Clear a slot for reuse (drops its decoder, clears its bytes).
    pub fn reset_slot(&self, slot: i32) -> Result<(), String> {
        let slot = Self::check_slot(slot)?;
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        if inner.active == Some(slot) {
            return Err("cannot reset the active slot".to_string());
        }
        inner.slots[slot].reset();
        self.slot_bufs[slot].reset();
        Ok(())
    }

    /// A slot index Go may push the next track into, or -1 if none is free.
    pub fn free_slot(&self) -> i32 {
        let inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        for (i, slot) in inner.slots.iter().enumerate() {
            if Some(i) != inner.active && slot.is_free() {
                return i as i32;
            }
        }
        -1
    }

    // ----------------------------------------------------------- params ---

    fn with_inner<F, R>(&self, f: F) -> R
    where
        F: FnOnce(&mut EngineInner) -> R,
    {
        f(&mut self.inner.lock().unwrap_or_else(|e| e.into_inner()))
    }

    pub fn set_preset(&self, name: &str) -> Result<(), String> {
        let preset = preset(name).ok_or_else(|| format!("unknown preset '{name}'"))?;
        self.with_inner(|inner| {
            let volume = inner.targets.volume; // presets never touch volume
            inner.targets = preset;
            inner.targets.volume = volume;
        });
        Ok(())
    }

    pub fn set_speed(&self, speed: f64) -> Result<(), String> {
        if !speed.is_finite() {
            return Err("speed must be finite".to_string());
        }
        self.with_inner(|inner| inner.targets.speed = speed.clamp(SPEED_MIN, SPEED_MAX));
        Ok(())
    }

    pub fn set_bass_db(&self, db: f64) -> Result<(), String> {
        if !db.is_finite() {
            return Err("bass_db must be finite".to_string());
        }
        self.with_inner(|inner| inner.targets.bass_db = db.clamp(BASS_MIN_DB, BASS_MAX_DB));
        Ok(())
    }

    pub fn set_volume(&self, volume: f64) -> Result<(), String> {
        if !volume.is_finite() {
            return Err("volume must be finite".to_string());
        }
        self.with_inner(|inner| inner.targets.volume = volume.clamp(VOLUME_MIN, VOLUME_MAX));
        Ok(())
    }

    pub fn set_loop(&self, mode: i32) -> Result<(), String> {
        let mode = LoopMode::from_i32(mode).ok_or_else(|| format!("invalid loop mode {mode}"))?;
        self.with_inner(|inner| inner.loop_mode = mode);
        Ok(())
    }

    pub fn set_echo(&self, enabled: bool, delay_ms: f64, decay: f64) -> Result<(), String> {
        if !delay_ms.is_finite() || !decay.is_finite() {
            return Err("echo parameters must be finite".to_string());
        }
        self.with_inner(|inner| {
            inner.targets.echo_on = enabled;
            inner.targets.echo_delay_ms = delay_ms.clamp(1.0, 500.0);
            inner.targets.echo_decay = decay.clamp(0.0, 0.95);
        });
        Ok(())
    }

    pub fn set_reverb(&self, enabled: bool, wet: f64) -> Result<(), String> {
        if !wet.is_finite() {
            return Err("reverb wet must be finite".to_string());
        }
        self.with_inner(|inner| {
            inner.targets.reverb_on = enabled;
            inner.targets.reverb_wet = wet.clamp(0.0, 1.0);
        });
        Ok(())
    }

    pub fn set_8d(&self, enabled: bool) -> Result<(), String> {
        self.with_inner(|inner| inner.targets.pan8d_on = enabled);
        Ok(())
    }

    // ------------------------------------------------------------ pull ----

    /// Pull one 960-frame as interleaved f32. Returns (code, transition).
    /// `out` must hold 1920 floats. Code: 0 = frame, 1 = need input,
    /// 2 = end of stream. On error returns Err(last_error).
    pub fn pull_f32(&self, out: &mut [f32]) -> Result<(i32, i32), String> {
        if out.len() < FRAME_SAMPLES * 2 {
            return Err("output buffer too small (need 1920 floats)".to_string());
        }
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        match inner.pull_frame()? {
            FrameOut::Frame { transition } => {
                for i in 0..FRAME_SAMPLES {
                    out[i * 2] = inner.out_l[i];
                    out[i * 2 + 1] = inner.out_r[i];
                }
                Ok((PULL_OK, transition))
            }
            FrameOut::NeedInput => Ok((PULL_NEED_INPUT, 0)),
            FrameOut::Eos => Ok((PULL_EOS, 0)),
        }
    }

    /// Pull one 960-frame as interleaved s16 (TPDF dithered). Returns
    /// (code, transition). `out` must hold 1920 samples.
    pub fn pull_s16(&self, out: &mut [i16]) -> Result<(i32, i32), String> {
        if out.len() < FRAME_SAMPLES * 2 {
            return Err("output buffer too small (need 1920 int16)".to_string());
        }
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        match inner.pull_frame()? {
            FrameOut::Frame { transition } => {
                for i in 0..FRAME_SAMPLES {
                    out[i * 2] = crate::fx::dither_s16(inner.out_l[i], &mut inner.fx.rng);
                    out[i * 2 + 1] = crate::fx::dither_s16(inner.out_r[i], &mut inner.fx.rng);
                }
                Ok((PULL_OK, transition))
            }
            FrameOut::NeedInput => Ok((PULL_NEED_INPUT, 0)),
            FrameOut::Eos => Ok((PULL_EOS, 0)),
        }
    }

    pub fn stats(&self) -> Stats {
        let inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        let active = inner.active.map(|a| a as i32).unwrap_or(-1);
        let free = (0..2)
            .find(|&i| Some(i) != inner.active && inner.slots[i].is_free())
            .map(|i| i as i32)
            .unwrap_or(-1);
        let (rate, channels) = match inner.active {
            Some(a) => (
                inner.slots[a].sample_rate() as i32,
                inner.slots[a].channels() as i32,
            ),
            None => (0, 0),
        };
        Stats {
            state: inner.state as i32,
            active_slot: active,
            free_slot: free,
            loop_mode: inner.loop_mode as i32,
            frames_pulled: inner.frames_pulled,
            samples_pulled: inner.samples_pulled,
            transitions: inner.transitions,
            loops: inner.loops,
            underruns: inner.underruns,
            speed: inner.currents.speed,
            bass_db: inner.currents.bass_db,
            volume: inner.targets.volume,
            input_sample_rate: rate,
            input_channels: channels,
        }
    }

    pub fn last_error(&self) -> String {
        self.inner.lock().unwrap_or_else(|e| e.into_inner()).last_error.clone()
    }
}

impl EngineInner {
    /// Produce one frame into `self.out_l` / `self.out_r`. Wraps
    /// `pull_frame_inner` so every error is recorded in `last_error`.
    fn pull_frame(&mut self) -> Result<FrameOut, String> {
        match self.pull_frame_inner() {
            Ok(out) => Ok(out),
            Err(e) => {
                self.last_error = e.clone();
                self.state = State::Error;
                Err(e)
            }
        }
    }

    fn pull_frame_inner(&mut self) -> Result<FrameOut, String> {
        // 0. Drop stale decoder state for slots whose buffer was reset
        //    for a new track since the last pull.
        self.slots[0].sync();
        self.slots[1].sync();
        // 1. Step smoothed parameters toward their targets.
        self.currents.step(&self.targets);
        let (bass_coeffs, bass_post) = self.currents.bass_coeffs();
        let vol_target = volume_gain(self.targets.volume) * bass_post;

        // 2. Push target-derived settings into the chain (cheap no-ops
        //    when unchanged; the chain ramps per-sample within the frame).
        self.fx.echo_l.set_params(0.8, 0.88, self.targets.echo_decay as f32);
        self.fx.echo_r.set_params(0.8, 0.88, self.targets.echo_decay as f32);
        self.fx
            .echo_l
            .set_delay(self.targets.echo_delay_ms as f32, 48_000.0);
        self.fx
            .echo_r
            .set_delay(self.targets.echo_delay_ms as f32, 48_000.0);
        self.fx.echo_l.set_wet(self.currents.echo_wet);
        self.fx.echo_r.set_wet(self.currents.echo_wet);
        self.fx.pan.set_amount(self.currents.pan_amount);
        self.fx.reverb.set_wet(self.currents.reverb_wet);

        // 3. Speed into the resampler (stepped by currents).
        if let Some(rs) = &mut self.resampler {
            if (rs.speed() - self.currents.speed).abs() > f64::EPSILON {
                rs.set_speed(self.currents.speed)?;
            }
        }

        // 4. Terminal / flush states.
        if self.flushing {
            return self.pull_flush(bass_coeffs, vol_target);
        }
        if self.state == State::Drained {
            return Ok(FrameOut::Eos);
        }

        // 5. Pick the active slot.
        if self.active.is_none() {
            if self.slots[0].is_loaded() {
                self.active = Some(0);
            } else if self.slots[1].is_loaded() {
                self.active = Some(1);
            } else {
                self.state = State::NeedInput;
                self.underruns += 1;
                return Ok(FrameOut::NeedInput);
            }
        }

        // 6. Fill the staging queue to one full resampler chunk, crossing
        //    track boundaries inside the chunk when the active slot drains.
        loop {
            let active = self.active.expect("active slot");
            // Ensure the slot is probed (decoding one packet probes it).
            if self.slots[active].sample_rate() == 0 && !self.slots[active].is_drained() {
                match self.slots[active].decode_more() {
                    Ok(SlotOutcome::Produced) => {}
                    Ok(SlotOutcome::NeedMoreInput) => {
                        self.state = State::NeedInput;
                        self.underruns += 1;
                        return Ok(FrameOut::NeedInput);
                    }
                    Ok(SlotOutcome::Drained) => {
                        // Probed but produced nothing (empty track).
                        if !self.advance_or_loop()? {
                            self.begin_flush();
                            return self.pull_flush(bass_coeffs, vol_target);
                        }
                        continue;
                    }
                    Err(e) => {
                        self.state = State::Error;
                        self.last_error = e.clone();
                        return Err(e);
                    }
                }
            }
            // Ensure the resampler matches the active slot's rate.
            let rate = self.slots[active].sample_rate();
            if rate == 0 {
                // Drained before probing (empty slot) — move on.
                if !self.advance_or_loop()? {
                    self.begin_flush();
                    return self.pull_flush(bass_coeffs, vol_target);
                }
                continue;
            }
            match &mut self.resampler {
                None => {
                    self.resampler = Some(Resampler48k::new(rate, self.currents.speed)?);
                }
                Some(rs) => {
                    if rs.in_rate() != rate {
                        rs.set_input_rate(rate)?;
                    }
                }
            }
            let need = self.resampler.as_ref().unwrap().input_frames_next();
            if self.staging_l.len() >= need {
                break;
            }
            // Take what the active slot offers.
            let avail = self.slots[active].available();
            if avail > 0 {
                let take = avail.min(need - self.staging_l.len());
                let base = self.staging_l.len();
                self.staging_l.resize(base + take, 0.0);
                self.staging_r.resize(base + take, 0.0);
                self.slots[active].take(
                    take,
                    &mut self.staging_l[base..],
                    &mut self.staging_r[base..],
                );
                continue;
            }
            // Nothing offered: decode more, or move on at EOF.
            match self.slots[active].decode_more() {
                Ok(SlotOutcome::Produced) => continue,
                Ok(SlotOutcome::NeedMoreInput) => {
                    self.state = State::NeedInput;
                    self.underruns += 1;
                    return Ok(FrameOut::NeedInput);
                }
                Ok(SlotOutcome::Drained) => {
                    if !self.advance_or_loop()? {
                        self.begin_flush();
                        return self.pull_flush(bass_coeffs, vol_target);
                    }
                    continue;
                }
                Err(e) => {
                    self.state = State::Error;
                    self.last_error = e.clone();
                    return Err(e);
                }
            }
        }

        // 7. Resample one chunk -> 960 frames. (Disjoint field borrows:
        //    resampler, staging, and scratch outputs.)
        let need = self.resampler.as_ref().unwrap().input_frames_next();
        let consumed = {
            let rs = self.resampler.as_mut().unwrap();
            let mut out_l = std::mem::take(&mut self.out_l);
            let mut out_r = std::mem::take(&mut self.out_r);
            let consumed = rs.process(&self.staging_l, &self.staging_r, &mut out_l, &mut out_r)?;
            self.out_l = out_l;
            self.out_r = out_r;
            consumed
        };
        debug_assert_eq!(consumed, need);
        self.staging_l.drain(..consumed);
        self.staging_r.drain(..consumed);

        // 8. FX chain.
        self.fx
            .process(&mut self.out_l, &mut self.out_r, bass_coeffs, vol_target);

        self.state = State::Playing;
        self.frames_pulled += 1;
        self.samples_pulled += FRAME_SAMPLES as u64;
        let transition = std::mem::take(&mut self.pending_transition);
        Ok(FrameOut::Frame { transition })
    }

    /// Try to move to the other slot (promotion) or restart the active
    /// slot (loop). Returns false when the queue is over (caller flushes).
    fn advance_or_loop(&mut self) -> Result<bool, String> {
        let active = self.active.expect("active slot");
        let other = 1 - active;
        if self.slots[other].is_loaded() {
            // Promote: old slot is consumed; new slot becomes active.
            self.slots[active].mark_consumed();
            self.active = Some(other);
            self.transitions += 1;
            self.pending_transition = 1;
            Ok(true)
        } else if self.loop_mode == LoopMode::Track {
            // Loop: re-decode the same bytes from the top. The resampler
            // and FX state keep running — the boundary is continuous.
            self.slots[active].reset();
            self.loops += 1;
            self.pending_transition = 2;
            Ok(true)
        } else {
            Ok(false)
        }
    }

    fn begin_flush(&mut self) {
        self.flushing = true;
        self.flush_calls = 0;
    }

    /// End-of-stream: drain the resampler's tail through the FX chain.
    /// Returns EOS once the resampler output is all zeros (or after a
    /// safety cap of flush calls).
    fn pull_flush(
        &mut self,
        bass_coeffs: crate::fx::BiquadCoeffs,
        vol_target: f32,
    ) -> Result<FrameOut, String> {
        let rs = match &mut self.resampler {
            Some(rs) => rs,
            None => {
                self.flushing = false;
                self.state = State::Drained;
                return Ok(FrameOut::Eos);
            }
        };
        // Feed the staging remainder (if any), then empty slices.
        let in_l: Vec<f32> = std::mem::take(&mut self.staging_l);
        let in_r: Vec<f32> = std::mem::take(&mut self.staging_r);
        let mut out_l = vec![0.0f32; FRAME_SAMPLES];
        let mut out_r = vec![0.0f32; FRAME_SAMPLES];
        rs.process_partial(&in_l, &in_r, &mut out_l, &mut out_r)?;
        self.flush_calls += 1;
        let peak = out_l
            .iter()
            .chain(out_r.iter())
            .fold(0.0f32, |m, &x| m.max(x.abs()));
        if peak == 0.0 || self.flush_calls > 8 {
            self.flushing = false;
            self.state = State::Drained;
            return Ok(FrameOut::Eos);
        }
        self.out_l.copy_from_slice(&out_l);
        self.out_r.copy_from_slice(&out_r);
        self.fx
            .process(&mut self.out_l, &mut self.out_r, bass_coeffs, vol_target);
        self.state = State::Playing;
        self.frames_pulled += 1;
        self.samples_pulled += FRAME_SAMPLES as u64;
        let transition = std::mem::take(&mut self.pending_transition);
        Ok(FrameOut::Frame { transition })
    }
}
