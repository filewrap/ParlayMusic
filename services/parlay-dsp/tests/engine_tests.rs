//! Integration tests for parlay-dsp, exercised through the same C-ABI Go
//! will use. Fixtures are generated with ffmpeg (and one hand-written f32
//! WAV for the sub-LSB detail test) into a per-test temp dir.
//!
//! The tests print their measurements (`cargo test -- --nocapture`) and
//! assert on them; the numbers are also recorded in
//! `phase5/worker-b-dsp.md`.

use parlay_dsp::*;
use std::ffi::CString;
use std::path::{Path, PathBuf};
use std::process::Command;

// Tests share the process; the C-ABI registry is global but handles are
// per-test, so no serialisation is needed. Fixture generation shells out
// to ffmpeg, which is fine in parallel (unique dirs per test).
static FFMPEG: &str = "ffmpeg";

fn tmpdir(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!(
        "parlay-dsp-test-{}-{}-{}",
        std::process::id(),
        name,
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

fn ffmpeg(args: &[&str]) {
    let out = Command::new(FFMPEG)
        .args(["-y", "-v", "error"])
        .args(args)
        .output()
        .expect("run ffmpeg");
    assert!(
        out.status.success(),
        "ffmpeg {:?} failed: {}",
        args,
        String::from_utf8_lossy(&out.stderr)
    );
}

/// 440 Hz sine, stereo, `dur_s` seconds, at `rate` Hz, in `fmt`.
/// Uses lavfi sine (sample-accurate, deterministic).
fn sine_fixture(dir: &Path, name: &str, rate: u32, dur_s: u32, fmt: &str) -> PathBuf {
    let path = dir.join(name);
    let src = format!("sine=frequency=440:duration={dur_s}:sample_rate={rate}");
    ffmpeg(&[
        "-f", "lavfi", "-i", &src, "-ac", "2", "-ar", &rate.to_string(), "-c:a", fmt,
        path.to_str().unwrap(),
    ]);
    assert!(path.is_file(), "fixture missing: {}", path.display());
    path
}

/// Hand-written f32 WAV: 440 Hz @ 0.5 + 12 kHz @ `fine_amp`, stereo,
/// 44.1 kHz. Exact sample control (ffmpeg cannot emit sub-LSB detail
/// reliably through its filter graph).
fn fine_detail_wav(dir: &Path, fine_amp: f32) -> PathBuf {
    let path = dir.join("fine.wav");
    let rate: u32 = 44100;
    let n = (rate * 4) as usize; // 4 s
    let mut samples = Vec::with_capacity(n * 2);
    for i in 0..n {
        let t = i as f32 / rate as f32;
        let v = 0.5 * (2.0 * std::f32::consts::PI * 440.0 * t).sin()
            + fine_amp * (2.0 * std::f32::consts::PI * 12000.0 * t).sin();
        samples.push(v);
        samples.push(v);
    }
    // Minimal WAV header: PCM float (format 3), stereo, f32.
    let data_bytes = (samples.len() * 4) as u32;
    let mut wav = Vec::with_capacity(44 + data_bytes as usize);
    wav.extend_from_slice(b"RIFF");
    wav.extend_from_slice(&(36 + data_bytes).to_le_bytes());
    wav.extend_from_slice(b"WAVEfmt ");
    wav.extend_from_slice(&16u32.to_le_bytes());
    wav.extend_from_slice(&3u16.to_le_bytes()); // IEEE float
    wav.extend_from_slice(&2u16.to_le_bytes());
    wav.extend_from_slice(&rate.to_le_bytes());
    wav.extend_from_slice(&(rate * 8).to_le_bytes());
    wav.extend_from_slice(&8u16.to_le_bytes());
    wav.extend_from_slice(&32u16.to_le_bytes());
    wav.extend_from_slice(b"data");
    wav.extend_from_slice(&data_bytes.to_le_bytes());
    for s in &samples {
        wav.extend_from_slice(&s.to_le_bytes());
    }
    std::fs::write(&path, wav).unwrap();
    path
}

fn silence_wav(dir: &Path) -> PathBuf {
    let path = dir.join("silence.wav");
    ffmpeg(&[
        "-f", "lavfi", "-i", "anullsrc=channel_layout=stereo:sample_rate=44100",
        "-t", "2", "-c:a", "pcm_s16le", path.to_str().unwrap(),
    ]);
    path
}

// ------------------------------------------------------------- engine driver ---

struct Driver {
    handle: u64,
}

impl Driver {
    fn new() -> Self {
        let mut h: u64 = 0;
        assert_eq!(unsafe { parlay_dsp_create(&mut h) }, 0);
        assert_ne!(h, 0);
        Driver { handle: h }
    }

    fn push_file(&self, slot: i32, path: &Path) {
        let bytes = std::fs::read(path).unwrap();
        assert_eq!(
            unsafe { parlay_dsp_push(self.handle, slot, bytes.as_ptr(), bytes.len()) },
            0
        );
        assert_eq!(unsafe { parlay_dsp_finish_input(self.handle, slot) }, 0);
    }

    fn set_preset(&self, name: &str) {
        let c = CString::new(name).unwrap();
        assert_eq!(unsafe { parlay_dsp_set_preset(self.handle, c.as_ptr()) }, 0);
    }

    /// Pull frames until EOS or NeedInput (with a bounded retry loop for
    /// the streaming case). Returns (samples per channel, transitions seen).
    fn pull_all_f32(&self, max_frames: usize) -> (Vec<f32>, Vec<i32>) {
        let mut out = Vec::new();
        let mut transitions = Vec::new();
        let mut buf = [0f32; 1920];
        let mut need_input_streak = 0;
        for _ in 0..max_frames {
            let mut tr = 0i32;
            let rc = unsafe { parlay_dsp_pull_f32(self.handle, buf.as_mut_ptr(), &mut tr) };
            match rc {
                0 => {
                    need_input_streak = 0;
                    // Store left channel only for waveform checks (plus
                    // keep a full copy in `out` as interleaved).
                    out.extend_from_slice(&buf);
                    if tr != 0 {
                        transitions.push(tr);
                    }
                }
                1 => {
                    need_input_streak += 1;
                    if need_input_streak > 200 {
                        panic!("stuck on NEED_INPUT (200 retries)");
                    }
                    std::thread::sleep(std::time::Duration::from_millis(2));
                }
                2 => break,
                e => {
                    let mut err = [0i8; 512];
                    let n = unsafe { parlay_dsp_last_error(self.handle, err.as_mut_ptr(), err.len()) };
                    let msg = if n > 0 {
                        String::from_utf8_lossy(
                            &err[..n as usize]
                                .iter()
                                .map(|&c| c as u8)
                                .collect::<Vec<_>>(),
                        )
                        .into_owned()
                    } else {
                        String::new()
                    };
                    panic!("pull failed rc={e}: {msg}");
                }
            }
        }
        (out, transitions)
    }

    fn stats(&self) -> ParlayDspStats {
        let mut st = ParlayDspStats {
            state: 0,
            active_slot: 0,
            free_slot: 0,
            loop_mode: 0,
            frames_pulled: 0,
            samples_pulled: 0,
            transitions: 0,
            loops: 0,
            underruns: 0,
            speed: 0.0,
            bass_db: 0.0,
            volume: 0.0,
            input_sample_rate: 0,
            input_channels: 0,
        };
        assert_eq!(
            unsafe { parlay_dsp_stats(self.handle, &mut st) },
            0,
            "stats failed"
        );
        st
    }
}

impl Drop for Driver {
    fn drop(&mut self) {
        unsafe { parlay_dsp_destroy(self.handle) };
    }
}

/// RMS of interleaved stereo, left channel.
fn rms_left(interleaved: &[f32]) -> f32 {
    let n = interleaved.len() / 2;
    let sum: f32 = interleaved.iter().step_by(2).map(|x| x * x).sum();
    (sum / n as f32).sqrt()
}

/// Max absolute sample in interleaved buffer.
fn max_abs(interleaved: &[f32]) -> f32 {
    interleaved.iter().fold(0.0f32, |m, &x| m.max(x.abs()))
}

/// Goertzel magnitude of a single frequency in the left channel.
fn goertzel_left(interleaved: &[f32], freq: f32, rate: f32) -> f32 {
    let w = 2.0 * std::f32::consts::PI * freq / rate;
    let cw = w.cos();
    let coeff = 2.0 * cw;
    let (mut s1, mut s2) = (0.0f32, 0.0f32);
    let mut n = 0usize;
    for (i, &x) in interleaved.iter().enumerate() {
        if i % 2 == 0 {
            let s0 = x + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
            n += 1;
        }
    }
    let power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    power.sqrt() / n as f32 * 2.0 // ~ amplitude of the component
}

/// Max |x[n] - x[n-1]| step in the left channel over a sample window.
fn max_step_left(interleaved: &[f32], from: usize, to: usize) -> f32 {
    let mut m = 0.0f32;
    let mut prev: Option<f32> = None;
    for i in from..to.min(interleaved.len() / 2) {
        let x = interleaved[i * 2];
        if let Some(p) = prev {
            m = m.max((x - p).abs());
        }
        prev = Some(x);
    }
    m
}

fn count_zero_crossings_left(interleaved: &[f32], from: usize, to: usize) -> usize {
    let mut count = 0;
    let mut prev: Option<f32> = None;
    for i in from..to.min(interleaved.len() / 2) {
        let x = interleaved[i * 2];
        if let Some(p) = prev {
            if (p < 0.0) != (x < 0.0) {
                count += 1;
            }
        }
        prev = Some(x);
    }
    count
}

// ------------------------------------------------------------------ tests ---

#[test]
fn decode_all_formats() {
    let dir = tmpdir("formats");
    let cases: [(&str, u32, &str); 6] = [
        ("tone.wav", 44100, "pcm_s16le"),
        ("tone.flac", 44100, "flac"),
        ("tone.mp3", 44100, "libmp3lame"),
        ("tone.m4a", 44100, "aac"),
        ("tone.ogg", 44100, "libvorbis"),
        ("tone.opus", 48000, "libopus"),
    ];
    for (name, rate, codec) in cases {
        let path = sine_fixture(&dir, name, rate, 4, codec);
        let d = Driver::new();
        d.push_file(0, &path);
        let (out, transitions) = d.pull_all_f32(300);
        let st = d.stats();
        let frames = out.len() / 1920;
        let samples = out.len() / 2;
        let rms = rms_left(&out);
        println!(
            "{name}: frames={frames} samples/ch={samples} rms={rms:.4} rate={} transitions={transitions:?} state={}",
            st.input_sample_rate, st.state
        );
        assert!(transitions.is_empty(), "{name}: unexpected transition");
        // lavfi sine amplitude is 0.125 (RMS 0.0884); just check it is
        // clearly audible and well under the limiter ceiling.
        assert!(rms > 0.05, "{name}: output too quiet (rms {rms})");
        assert!(rms < 0.2, "{name}: output too loud (rms {rms})");
        // 4 s at 48 kHz = 192000 samples = 200 frames, +/- 2 for codec trim.
        assert!(
            (198..=202).contains(&frames),
            "{name}: expected ~200 frames, got {frames}"
        );
        assert_eq!(st.input_sample_rate, rate as i32, "{name}: rate mismatch");
        assert_eq!(st.state, 3, "{name}: expected Drained state");
    }
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn opus_sample_count_matches_ffmpeg() {
    // The granule/pre-skip trim is exact iff the engine emits the same
    // number of samples ffmpeg's libopus decode produces.
    let dir = tmpdir("opus-count");
    let path = sine_fixture(&dir, "tone.opus", 48000, 4, "libopus");
    // ffmpeg reference decode to f32le 48 kHz.
    let ref_raw = dir.join("ref.f32");
    ffmpeg(&[
        "-i",
        path.to_str().unwrap(),
        "-f",
        "f32le",
        "-ac",
        "2",
        "-ar",
        "48000",
        ref_raw.to_str().unwrap(),
    ]);
    let ref_bytes = std::fs::read(&ref_raw).unwrap();
    let ref_samples = ref_bytes.len() / 8; // stereo f32
    let d = Driver::new();
    d.push_file(0, &path);
    let (out, _) = d.pull_all_f32(300);
    let st = d.stats();
    let got = st.samples_pulled as usize;
    println!("opus: ffmpeg samples/ch={ref_samples} engine samples/ch={got} diff={}", got as i64 - ref_samples as i64);
    // Within one 960-frame of the reference (flush granularity).
    assert!(
        (got as i64 - ref_samples as i64).abs() <= 960,
        "opus sample count off by more than one frame"
    );
    // Waveform must match ffmpeg's decode closely (same libopus) over
    // the steady middle. Two known, constant offsets are accounted for:
    // * the limiter's 5 ms lookahead delay (240 samples/ch): the engine
    //   output is ref delayed by exactly 240 samples, so compare ref[i]
    //   against out[i + 480] (interleaved);
    // * the resampler startup transient (first ~1000 samples) and the
    //   final flush frame are skipped.
    let ref_f32: Vec<f32> = ref_bytes
        .as_chunks::<4>()
        .0
        .iter()
        .map(|c| f32::from_le_bytes(*c))
        .collect();
    let delay = 480usize; // 240 samples/ch, interleaved
    let n = (ref_f32.len().min(out.len().saturating_sub(delay))).saturating_sub(1920);
    let mut max_diff = 0.0f32;
    for i in 2000..n {
        max_diff = max_diff.max((ref_f32[i] - out[i + delay]).abs());
    }
    println!("opus: max |engine - ffmpeg| over samples 2000..{n} (240-sample delay compensated) = {max_diff:.6}");
    assert!(max_diff < 0.01, "opus waveform diverges from ffmpeg decode");
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn gapless_two_track_exact() {
    // Two identical 2 s halves (440 Hz = 880 whole periods per 2 s, so the
    // concatenation is a continuous sine) vs the 4 s file played as one
    // track. The outputs must be bit-identical: zero silence/click.
    let dir = tmpdir("gapless");
    let half = sine_fixture(&dir, "half.flac", 44100, 2, "flac");
    let full = sine_fixture(&dir, "full.flac", 44100, 4, "flac");

    let d = Driver::new();
    d.push_file(0, &half);
    d.push_file(1, &half);
    let (two, tr) = d.pull_all_f32(300);
    let st_two = d.stats();

    let d2 = Driver::new();
    d2.push_file(0, &full);
    let (one, _) = d2.pull_all_f32(300);

    assert_eq!(tr, vec![1], "expected exactly one track transition, got {tr:?}");
    assert_eq!(st_two.transitions, 1);
    assert_eq!(two.len(), one.len(), "length mismatch: two-track {} vs one-track {}", two.len(), one.len());
    let mut max_diff = 0.0f32;
    for i in 0..two.len() {
        max_diff = max_diff.max((two[i] - one[i]).abs());
    }
    println!("gapless flac: len={} samples max|two-one|={max_diff}", two.len() / 2);
    assert_eq!(max_diff, 0.0, "gapless handoff is not sample-accurate");

    // Same check for WAV (no codec delay/padding at all).
    let half_wav = sine_fixture(&dir, "half.wav", 44100, 2, "pcm_s16le");
    let full_wav = sine_fixture(&dir, "full.wav", 44100, 4, "pcm_s16le");
    let d3 = Driver::new();
    d3.push_file(0, &half_wav);
    d3.push_file(1, &half_wav);
    let (two_w, tr_w) = d3.pull_all_f32(300);
    let d4 = Driver::new();
    d4.push_file(0, &full_wav);
    let (one_w, _) = d4.pull_all_f32(300);
    assert_eq!(tr_w, vec![1]);
    let mut max_diff_w = 0.0f32;
    for i in 0..two_w.len() {
        max_diff_w = max_diff_w.max((two_w[i] - one_w[i]).abs());
    }
    println!("gapless wav: max|two-one|={max_diff_w}");
    assert_eq!(max_diff_w, 0.0, "wav gapless handoff not sample-accurate");
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn gapless_loop_exact() {
    // Loop a 2 s sine twice; must equal the 4 s file played once.
    let dir = tmpdir("loop");
    let half = sine_fixture(&dir, "half.flac", 44100, 2, "flac");
    let full = sine_fixture(&dir, "full.flac", 44100, 4, "flac");
    let d = Driver::new();
    assert_eq!(unsafe { parlay_dsp_set_loop(d.handle, 1) }, 0);
    d.push_file(0, &half);
    // Pull 199 frames then stop: loop is infinite otherwise. (Pulling the
    // 200th frame would trigger a 2nd restart: the resampler's lookahead
    // reaches into the next loop while filling that chunk.)
    let mut buf = [0f32; 1920];
    let mut out = Vec::new();
    let mut transitions = Vec::new();
    for _ in 0..199 {
        let mut tr = 0i32;
        let rc = unsafe { parlay_dsp_pull_f32(d.handle, buf.as_mut_ptr(), &mut tr) };
        assert_eq!(rc, 0, "loop pull rc={rc}");
        out.extend_from_slice(&buf);
        if tr != 0 {
            transitions.push(tr);
        }
    }
    let st = d.stats();
    let d2 = Driver::new();
    d2.push_file(0, &full);
    let (one_full, _) = d2.pull_all_f32(300);
    let one: Vec<f32> = one_full[..out.len()].to_vec();
    assert_eq!(transitions, vec![2], "expected one loop restart, got {transitions:?}");
    assert_eq!(st.loops, 1);
    let mut max_diff = 0.0f32;
    for i in 0..out.len() {
        max_diff = max_diff.max((out[i] - one[i]).abs());
    }
    println!("loop: max|loop-once|={max_diff} transitions={transitions:?}");
    assert_eq!(max_diff, 0.0, "loop restart is not sample-accurate");
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn gapless_cross_rate() {
    // 44.1 kHz half -> 48 kHz half: promotion must work across a rate
    // change (ratio update, no resampler reset), with no error and no
    // silence at the boundary.
    let dir = tmpdir("xrate");
    let half44 = sine_fixture(&dir, "half44.flac", 44100, 2, "flac");
    let half48 = sine_fixture(&dir, "half48.flac", 48000, 2, "flac");
    let d = Driver::new();
    d.push_file(0, &half44);
    d.push_file(1, &half48);
    let (out, tr) = d.pull_all_f32(300);
    assert_eq!(tr, vec![1], "expected one transition, got {tr:?}");
    assert!(
        (200..=201).contains(&(out.len() / 1920)),
        "expected 200-201 frames, got {}",
        out.len() / 1920
    );
    // No run of >5 exact zeros anywhere past the start (would indicate
    // inserted silence). The first 240 samples are the limiter's 5 ms
    // lookahead delay line flushing — exact zeros by design, skipped.
    let mut run = 0usize;
    let mut max_run = 0usize;
    for i in 1000..out.len() / 2 {
        if out[i * 2] == 0.0 {
            run += 1;
            max_run = max_run.max(run);
        } else {
            run = 0;
        }
    }
    println!("cross-rate: frames={} max zero run={max_run}", out.len() / 1920);
    assert!(max_run <= 5, "silence inserted at the rate-change boundary");
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn zero_drop_speed_switch() {
    let dir = tmpdir("speed");
    let path = sine_fixture(&dir, "tone.flac", 44100, 4, "flac");
    let d = Driver::new();
    d.push_file(0, &path);
    let mut buf = [0f32; 1920];
    let mut out = Vec::new();
    let mut switch_frame = 0usize;
    // At 1.25x the 4 s track ends at frame 160; pull 150 frames.
    for frame in 0..150 {
        if frame == 50 {
            assert_eq!(unsafe { parlay_dsp_set_speed(d.handle, 1.25) }, 0);
            switch_frame = frame;
        }
        let mut tr = 0i32;
        let rc = unsafe { parlay_dsp_pull_f32(d.handle, buf.as_mut_ptr(), &mut tr) };
        assert_eq!(rc, 0);
        out.extend_from_slice(&buf);
    }
    assert!(!max_abs(&out).is_nan());
    assert!(out.iter().all(|x| x.is_finite()), "non-finite sample produced");
    // Switch point in samples (left channel): frame 50 * 960.
    let sp = switch_frame * 960;
    let step_at = max_step_left(&out, sp.saturating_sub(100), sp + 100);
    let step_before = max_step_left(&out, sp - 5000, sp - 100);
    let step_after = max_step_left(&out, sp + 2000, sp + 5000);
    println!("speed switch: step@switch={step_at:.5} step_before={step_before:.5} step_after={step_after:.5}");
    assert!(step_at < 0.05, "click at speed switch (max step {step_at})");
    assert!(
        step_at < 3.0 * step_before.max(step_after),
        "switch step {step_at} dwarfs surrounding steps"
    );
    // The speed effect must actually work: zero-crossing rate up ~1.25x.
    let zc_before = count_zero_crossings_left(&out, sp - 24000, sp);
    let zc_after = count_zero_crossings_left(&out, sp + 9600, sp + 33600);
    let ratio = zc_after as f32 / zc_before as f32;
    println!("speed switch: zc before={zc_before} after={zc_after} ratio={ratio:.3} (expect ~1.25)");
    assert!((ratio - 1.25).abs() < 0.05, "speed change did not shift pitch by 1.25x");
    let st = d.stats();
    assert!((st.speed - 1.25).abs() < 1e-9, "stats speed not settled at 1.25");
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn zero_drop_bass_switch() {
    // Fixture: 30 Hz + 1000 Hz mixed at equal amplitude (ffmpeg aevalsrc).
    // 30 Hz sits deep in the shelf plateau (the RBJ shelf with slope 0.6
    // is still transitioning at 60 Hz).
    let dir = tmpdir("bass");
    let path = dir.join("mix.flac");
    ffmpeg(&[
        "-f", "lavfi", "-i",
        "aevalsrc=0.3*sin(2*PI*30*t)+0.3*sin(2*PI*1000*t):duration=4:sample_rate=44100",
        "-ac", "2", "-c:a", "flac", path.to_str().unwrap(),
    ]);
    let d = Driver::new();
    d.push_file(0, &path);
    let mut buf = [0f32; 1920];
    let mut out = Vec::new();
    let mut switch_frame = 0usize;
    for frame in 0..200 {
        if frame == 50 {
            assert_eq!(unsafe { parlay_dsp_set_bass_db(d.handle, 10.0) }, 0);
            switch_frame = frame;
        }
        let mut tr = 0i32;
        let rc = unsafe { parlay_dsp_pull_f32(d.handle, buf.as_mut_ptr(), &mut tr) };
        assert_eq!(rc, 0);
        out.extend_from_slice(&buf);
    }
    let sp = switch_frame * 960;
    let step_at = max_step_left(&out, sp.saturating_sub(100), sp + 100);
    let step_before = max_step_left(&out, sp - 5000, sp - 100);
    println!("bass switch: step@switch={step_at:.5} step_before={step_before:.5}");
    assert!(step_at < 0.05, "click at bass switch (max step {step_at})");
    assert!(step_at < 3.0 * step_before, "switch step dwarfs surrounding steps");
    // Bass must actually boost lows: Goertzel at 30 Hz vs 1000 Hz on
    // clean 1 s segments (head = pre-switch, tail = post-switch, well
    // after the 5-frame ramp). Expect the +10 dB shelf minus the 0.9
    // post-gain (~+9.1 dB) at 30 Hz.
    let head: Vec<f32> = out[..48000 * 2].to_vec();
    let g30_before = goertzel_left(&head, 30.0, 48000.0);
    let g1000_before = goertzel_left(&head, 1000.0, 48000.0);
    let tail: Vec<f32> = out[out.len() - 48000 * 2..].to_vec();
    let g30_after = goertzel_left(&tail, 30.0, 48000.0);
    let g1000_after = goertzel_left(&tail, 1000.0, 48000.0);
    let boost_db = 20.0 * (g30_after / g30_before).log10();
    let hf_db = 20.0 * (g1000_after / g1000_before).log10();
    println!("bass: 30Hz {g30_before:.4} -> {g30_after:.4} ({boost_db:+.1} dB); 1kHz {g1000_before:.4} -> {g1000_after:.4} ({hf_db:+.1} dB)");
    assert!(boost_db > 7.0 && boost_db < 12.0, "bass boost not ~+9 dB (got {boost_db:+.1})");
    assert!(hf_db.abs() < 1.5, "bass shelf leaked into 1 kHz ({hf_db:+.1} dB)");
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn frame_size_exact() {
    let dir = tmpdir("frames");
    let path = sine_fixture(&dir, "tone.flac", 44100, 4, "flac");
    let d = Driver::new();
    d.push_file(0, &path);
    let (out, _) = d.pull_all_f32(300);
    let st = d.stats();
    assert_eq!(out.len() % 1920, 0, "output not a whole number of frames");
    assert_eq!(st.frames_pulled as usize, out.len() / 1920);
    assert_eq!(st.samples_pulled as usize, out.len() / 2);
    println!("frame size: {} frames, {} samples/ch, all 960", st.frames_pulled, st.samples_pulled);
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn f32_preserves_fine_detail() {
    // 12 kHz component at 1e-6 amplitude = 0.03 of a 16-bit LSB. Any
    // 16-bit intermediate would destroy it; the f32 path must preserve it.
    let dir = tmpdir("fine");
    let path = fine_detail_wav(&dir, 1e-6);
    let d = Driver::new();
    d.push_file(0, &path);
    let (out, _) = d.pull_all_f32(300);
    let g12k = goertzel_left(&out, 12000.0, 48000.0);
    println!("fine detail: Goertzel 12kHz amplitude = {g12k:.3e} (input 1e-6)");
    assert!(
        (g12k - 1e-6).abs() < 0.35e-6,
        "12 kHz fine detail lost or distorted (got {g12k:.3e}, want ~1e-6)"
    );
    // Direct check: samples must not be 16-bit quantised.
    let mut non_quantised = 0usize;
    let mut total = 0usize;
    for &x in out.iter().step_by(2) {
        if x != 0.0 {
            total += 1;
            let q = (x * 32768.0).round() / 32768.0;
            if (x - q).abs() > 1e-9 {
                non_quantised += 1;
            }
        }
    }
    let frac = non_quantised as f64 / total.max(1) as f64;
    println!("fine detail: {non_quantised}/{total} nonzero samples carry sub-LSB info ({:.1}%)", frac * 100.0);
    assert!(frac > 0.99, "output looks 16-bit quantised");
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn s16_path_is_dithered() {
    // Silence through the f32 path must be exactly 0.0 (no dither there);
    // through the s16 path it must be TPDF noise (+/-1 LSB), proving the
    // dither lives only at the final conversion.
    let dir = tmpdir("dither");
    let path = silence_wav(&dir);
    let d = Driver::new();
    d.push_file(0, &path);
    let mut buf_f = [0f32; 1920];
    let mut tr = 0i32;
    let rc = unsafe { parlay_dsp_pull_f32(d.handle, buf_f.as_mut_ptr(), &mut tr) };
    assert_eq!(rc, 0);
    assert!(buf_f.iter().all(|&x| x == 0.0), "f32 silence is not exact zero");

    let d2 = Driver::new();
    d2.push_file(0, &path);
    let mut buf_s = [0i16; 1920];
    let mut nonzero = 0usize;
    let mut min_v = i16::MAX;
    let mut max_v = i16::MIN;
    for _ in 0..50 {
        let mut tr2 = 0i32;
        let rc2 = unsafe { parlay_dsp_pull_s16(d2.handle, buf_s.as_mut_ptr(), &mut tr2) };
        assert_eq!(rc2, 0);
        for &s in &buf_s {
            if s != 0 {
                nonzero += 1;
            }
            min_v = min_v.min(s);
            max_v = max_v.max(s);
        }
    }
    let total = 50 * 1920;
    println!("s16 silence: {nonzero}/{total} nonzero, range [{min_v}, {max_v}]");
    assert!(nonzero * 10 > total, "s16 silence has no dither noise");
    assert!(min_v >= -1 && max_v <= 1, "dither exceeds 1 LSB");
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn chunked_push_streaming() {
    // Push in 4 KB chunks from another thread while pulling: exercises
    // the blocking-read path (no deadlock, no lost samples).
    let dir = tmpdir("stream");
    let path = sine_fixture(&dir, "tone.mp3", 44100, 4, "libmp3lame");
    let bytes = std::fs::read(&path).unwrap();
    let d = Driver::new();
    let handle = d.handle;
    let feeder = std::thread::spawn(move || {
        for chunk in bytes.chunks(4096) {
            assert_eq!(
                unsafe { parlay_dsp_push(handle, 0, chunk.as_ptr(), chunk.len()) },
                0
            );
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
        assert_eq!(unsafe { parlay_dsp_finish_input(handle, 0) }, 0);
    });
    let (out, _) = d.pull_all_f32(400);
    feeder.join().unwrap();
    let st = d.stats();
    let rms = rms_left(&out);
    println!("streaming: frames={} rms={rms:.4} underruns={}", st.frames_pulled, st.underruns);
    assert!(rms > 0.05, "streamed output too quiet");
    assert!((198..=202).contains(&(out.len() / 1920)), "frame count off");
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn first_frame_latency_and_throughput() {
    let dir = tmpdir("bench");
    let path = sine_fixture(&dir, "tone.flac", 44100, 10, "flac");
    // Cold first frame.
    let d = Driver::new();
    let t0 = std::time::Instant::now();
    d.push_file(0, &path);
    let mut buf = [0f32; 1920];
    let mut tr = 0i32;
    let rc = unsafe { parlay_dsp_pull_f32(d.handle, buf.as_mut_ptr(), &mut tr) };
    let first_frame = t0.elapsed();
    assert_eq!(rc, 0);
    println!("first-frame latency (cold, push+probe+decode+resample): {first_frame:?}");
    assert!(first_frame.as_millis() < 500, "first frame too slow: {first_frame:?}");

    // Throughput: pull 10 s of audio.
    let d2 = Driver::new();
    d2.push_file(0, &path);
    let t0 = std::time::Instant::now();
    let (out, _) = d2.pull_all_f32(600);
    let elapsed = t0.elapsed();
    let audio_s = out.len() as f64 / 2.0 / 48000.0;
    println!(
        "throughput: {:.2} s of audio in {:?} ({:.1}x realtime)",
        audio_s,
        elapsed,
        audio_s / elapsed.as_secs_f64()
    );
    assert!(
        audio_s / elapsed.as_secs_f64() > 5.0,
        "DSP slower than 5x realtime"
    );

    // Legacy ffmpeg pipeline for comparison (subprocess decode to s16le).
    let t0 = std::time::Instant::now();
    let out = Command::new(FFMPEG)
        .args([
            "-y", "-v", "error", "-i", path.to_str().unwrap(),
            "-f", "s16le", "-ac", "2", "-ar", "48000", "-",
        ])
        .output()
        .expect("run ffmpeg");
    let ff_elapsed = t0.elapsed();
    assert!(out.status.success());
    let ff_audio_s = out.stdout.len() as f64 / 4.0 / 48000.0;
    println!(
        "legacy ffmpeg pipeline: {:.2} s of audio in {:?} ({:.1}x realtime)",
        ff_audio_s,
        ff_elapsed,
        ff_audio_s / ff_elapsed.as_secs_f64()
    );
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn abi_validation() {
    let d = Driver::new();
    // Unknown preset.
    let bad = CString::new("chipmunk").unwrap();
    assert_eq!(unsafe { parlay_dsp_set_preset(d.handle, bad.as_ptr()) }, -3);
    // Bad slot.
    assert_eq!(unsafe { parlay_dsp_push(d.handle, 7, std::ptr::null(), 0) }, -3);
    // Bad loop mode.
    assert_eq!(unsafe { parlay_dsp_set_loop(d.handle, 9) }, -3);
    // Bad handle.
    let mut buf = [0f32; 1920];
    let mut tr = 0i32;
    assert_eq!(unsafe { parlay_dsp_pull_f32(0xFFFF_FFFF, buf.as_mut_ptr(), &mut tr) }, -2);
    // Null out pointer.
    assert_eq!(unsafe { parlay_dsp_pull_f32(d.handle, std::ptr::null_mut(), &mut tr) }, -3);
    // Preset applies and settles.
    d.set_preset("nightcore");
    let path = {
        let dir = tmpdir("preset");
        let p = sine_fixture(&dir, "tone.flac", 44100, 2, "flac");
        d.push_file(0, &p);
        let _ = std::fs::remove_dir_all(&dir);
        p
    };
    let _ = path;
    let mut buf2 = [0f32; 1920];
    for _ in 0..10 {
        let mut tr2 = 0i32;
        let rc = unsafe { parlay_dsp_pull_f32(d.handle, buf2.as_mut_ptr(), &mut tr2) };
        assert_eq!(rc, 0);
    }
    let st = d.stats();
    println!("preset nightcore: speed settled at {}", st.speed);
    assert!((st.speed - 1.25).abs() < 1e-9);
    // free_slot: slot 1 is free (slot 0 active), slot 0 is not.
    assert_eq!(unsafe { parlay_dsp_free_slot(d.handle) }, 1);
    // Resetting the active slot is an error.
    assert_eq!(unsafe { parlay_dsp_reset_slot(d.handle, 0) }, -3);
    assert_eq!(unsafe { parlay_dsp_reset_slot(d.handle, 1) }, 0);
}

