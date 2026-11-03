//! Resampling to 48 kHz stereo with rubato's `SincFixedOut`.
//!
//! Fixed 960-frame output (one 20 ms voice frame per call). The resample
//! ratio carries the **speed** effect: `ratio = (48000 / in_rate) / speed`,
//! so nightcore 1.25x plays faster *and* higher (vinyl-style), matching the
//! locked doc's "dynamic resampler ratio" design. rubato ramps the ratio
//! across a chunk when it changes (`set_resample_ratio(..., ramp = true)`),
//! and the engine additionally steps the speed target by at most 0.05 per
//! frame, so a speed change is a smooth pitch glide, never a step.
//!
//! Gapless note: the resampler is created once per input sample rate and
//! is **never reset at a track boundary**. When the next track has the same
//! rate, the resampler just keeps consuming — the boundary is exactly what
//! resampling the concatenation would produce. When the rate changes, only
//! the ratio is updated (ramped); the filter state carries over, so the
//! output stays continuous.

use rubato::{
    Resampler, SincFixedOut, SincInterpolationParameters, SincInterpolationType, WindowFunction,
};

pub const OUT_RATE: u32 = 48_000;
pub const FRAME_SAMPLES: usize = 960;

fn sinc_params() -> SincInterpolationParameters {
    SincInterpolationParameters {
        sinc_len: 256,
        f_cutoff: 0.95,
        interpolation: SincInterpolationType::Cubic,
        oversampling_factor: 256,
        window: WindowFunction::BlackmanHarris2,
    }
}

pub struct Resampler48k {
    inner: SincFixedOut<f32>,
    in_rate: u32,
    speed: f64,
    /// Scratch input buffers, sized to `input_frames_max()`.
    in_bufs: Vec<Vec<f32>>,
    /// Scratch output buffers, exactly 960 frames each.
    out_bufs: Vec<Vec<f32>>,
}

impl Resampler48k {
    pub fn new(in_rate: u32, speed: f64) -> Result<Self, String> {
        let ratio = (OUT_RATE as f64 / in_rate as f64) / speed;
        // max_relative 8.0: covers speed 0.5–2.0 (4x) AND an input-rate
        // change of up to ~4x (e.g. 44.1 kHz -> 192 kHz) via
        // set_resample_ratio without rebuilding the resampler.
        let inner = SincFixedOut::<f32>::new(ratio, 8.0, sinc_params(), FRAME_SAMPLES, 2)
            .map_err(|e| format!("resampler construction failed: {e}"))?;
        let in_bufs = inner.input_buffer_allocate(true);
        let out_bufs = inner.output_buffer_allocate(true);
        Ok(Resampler48k {
            inner,
            in_rate,
            speed,
            in_bufs,
            out_bufs,
        })
    }

    pub fn in_rate(&self) -> u32 {
        self.in_rate
    }

    pub fn speed(&self) -> f64 {
        self.speed
    }

    /// Input frames the resampler needs for the next 960-frame output.
    pub fn input_frames_next(&self) -> usize {
        self.inner.input_frames_next()
    }

    /// Update the speed (ratio). Ramped across the next chunk by rubato.
    pub fn set_speed(&mut self, speed: f64) -> Result<(), String> {
        let ratio = (OUT_RATE as f64 / self.in_rate as f64) / speed;
        self.inner
            .set_resample_ratio(ratio, true)
            .map_err(|e| format!("set_resample_ratio failed: {e}"))?;
        self.speed = speed;
        Ok(())
    }

    /// Switch to a different input sample rate (track boundary). Only the
    /// ratio is updated — the filter state carries over, so the output
    /// stays continuous. Ramped across the next chunk by rubato.
    pub fn set_input_rate(&mut self, in_rate: u32) -> Result<(), String> {
        if in_rate == self.in_rate {
            return Ok(());
        }
        let ratio = (OUT_RATE as f64 / in_rate as f64) / self.speed;
        self.inner
            .set_resample_ratio(ratio, true)
            .map_err(|e| format!("set_resample_ratio (rate change) failed: {e}"))?;
        self.in_rate = in_rate;
        Ok(())
    }

    /// Resample one chunk. `in_l`/`in_r` must hold at least
    /// `input_frames_next()` frames each; exactly 960 frames per channel
    /// are written to `out_l`/`out_r`. Returns the number of input frames
    /// consumed.
    pub fn process(
        &mut self,
        in_l: &[f32],
        in_r: &[f32],
        out_l: &mut [f32],
        out_r: &mut [f32],
    ) -> Result<usize, String> {
        let need = self.inner.input_frames_next();
        debug_assert!(in_l.len() >= need && in_r.len() >= need);
        debug_assert!(out_l.len() >= FRAME_SAMPLES && out_r.len() >= FRAME_SAMPLES);
        self.in_bufs[0][..need].copy_from_slice(&in_l[..need]);
        self.in_bufs[1][..need].copy_from_slice(&in_r[..need]);
        let mut outs: Vec<&mut [f32]> =
            self.out_bufs.iter_mut().map(|b| b.as_mut_slice()).collect();
        let (consumed, produced) = self
            .inner
            .process_into_buffer(&self.in_bufs, &mut outs, None)
            .map_err(|e| format!("resample failed: {e}"))?;
        debug_assert_eq!(produced, FRAME_SAMPLES);
        out_l[..produced].copy_from_slice(&outs[0][..produced]);
        out_r[..produced].copy_from_slice(&outs[1][..produced]);
        Ok(consumed)
    }

    /// Flush the resampler at end of stream: feed the remaining `in_*`
    /// samples (zero-padded) and produce one 960-frame chunk. When the
    /// input is exhausted, call with empty slices to push the delayed tail
    /// out. Returns the produced frames (always 960 on success).
    ///
    /// Note: rubato's `process_partial_into_buffer` must be called with
    /// `None` (not empty slices) when there is no input left — empty
    /// slices clear the padded buffer and fail validation.
    pub fn process_partial(
        &mut self,
        in_l: &[f32],
        in_r: &[f32],
        out_l: &mut [f32],
        out_r: &mut [f32],
    ) -> Result<usize, String> {
        let mut outs: Vec<&mut [f32]> =
            self.out_bufs.iter_mut().map(|b| b.as_mut_slice()).collect();
        let result = if in_l.is_empty() && in_r.is_empty() {
            self.inner
                .process_partial_into_buffer(None::<&[Vec<f32>]>, &mut outs, None)
        } else {
            let wave_in: Vec<&[f32]> = vec![in_l, in_r];
            self.inner
                .process_partial_into_buffer(Some(&wave_in), &mut outs, None)
        };
        let (_, produced) = result.map_err(|e| format!("resample flush failed: {e}"))?;
        out_l[..produced].copy_from_slice(&outs[0][..produced]);
        out_r[..produced].copy_from_slice(&outs[1][..produced]);
        Ok(produced)
    }
}
