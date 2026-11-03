//! One decode slot: a single track's bytes, decoder, and decoded-but-not-
//! yet-resampled stereo PCM.
//!
//! A slot is fed by `push` (bytes) + `finish_input` (Go), then decoded
//! lazily on first `decode_more`. Format detection order:
//! 1. Ogg-Opus sniff (first packet starts with `OpusHead`) -> `OggOpusDecoder`.
//! 2. Everything else -> symphonia probe (FLAC, MP3, AAC/M4A/ALAC, Vorbis,
//!    WAV/PCM, ...).
//!
//! Gapless trimming:
//! * symphonia path: `codec_params.delay` leading samples are skipped and
//!   `codec_params.padding` trailing samples are held back — the engine is
//!   only ever offered `pending.len() - padding` samples, so padding can
//!   never enter the resampler. (FLAC/WAV have delay = padding = 0.)
//! * Opus path: pre-skip + per-packet granule trim inside `OggOpusDecoder`,
//!   so its pending queue is already exact (holdback 0).

use crate::ogg_opus::{DetectOutcome, OggOpusDecoder, OggOutcome};
use crate::source::{BufReader, SharedBuf};
use std::sync::Arc;
use symphonia::core::audio::{AudioBufferRef, SampleBuffer, SignalSpec};
use symphonia::core::codecs::DecoderOptions;
use symphonia::core::formats::FormatOptions;
use symphonia::core::io::MediaSourceStream;
use symphonia::core::meta::MetadataOptions;
use symphonia::core::probe::Hint;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SlotOutcome {
    /// Decoded samples were appended to the pending queue.
    Produced,
    /// Decoder needs more input bytes (push more, then retry).
    NeedMoreInput,
    /// Track fully decoded (pending queue holds the exact tail).
    Drained,
}

enum Decoder {
    Unprobed,
    Symphonia {
        format: Box<dyn symphonia::core::formats::FormatReader>,
        track_id: u32,
        decoder: Box<dyn symphonia::core::codecs::Decoder>,
        sample_buf: SampleBuffer<f32>,
        spec: SignalSpec,
        /// Leading samples to skip (encoder delay, e.g. MP3).
        skip_remaining: u32,
        /// Trailing samples to hold back (encoder padding).
        holdback: usize,
        packets_decoded: u64,
    },
    Opus(OggOpusDecoder),
    /// Terminal failure (bad file). `decode_more` keeps returning this.
    Failed(String),
}

pub struct DecodeSlot {
    buf: Arc<SharedBuf>,
    decoder: Decoder,
    /// Decoded stereo PCM awaiting the resampler (post trim).
    pending_l: Vec<f32>,
    pending_r: Vec<f32>,
    /// True once the decoder hit EOF (pending holds the exact tail).
    drained: bool,
    /// True once any byte was pushed into this slot for the current track.
    loaded: bool,
    /// Input sample rate, known after probing (0 = unknown).
    sample_rate: u32,
    /// Input channel count, known after probing (0 = unknown).
    channels: u32,
    /// Buffer generation last seen. A bump means a new track was pushed
    /// into this slot: decoder state, `drained`, and queues are stale and
    /// must be reset (see `sync`).
    seen_generation: u64,
}

impl DecodeSlot {
    pub fn new(buf: Arc<SharedBuf>) -> Self {
        let seen_generation = buf.generation();
        DecodeSlot {
            buf,
            decoder: Decoder::Unprobed,
            pending_l: Vec::new(),
            pending_r: Vec::new(),
            drained: false,
            loaded: false,
            sample_rate: 0,
            channels: 0,
            seen_generation,
        }
    }

    /// If the shared buffer was reset for a new track since this slot last
    /// looked, drop all stale decoder state. Called by the engine at the
    /// top of every pull (the push path never touches the slot).
    pub fn sync(&mut self) {
        let gen = self.buf.generation();
        if gen != self.seen_generation {
            self.seen_generation = gen;
            self.decoder = Decoder::Unprobed;
            self.pending_l.clear();
            self.pending_r.clear();
            self.drained = false;
            self.loaded = false;
            self.sample_rate = 0;
            self.channels = 0;
        }
    }

    pub fn is_loaded(&self) -> bool {
        !self.buf.is_consumed() && (self.loaded || self.buf.len() > 0)
    }

    /// Free for Go to push the next track into: consumed, or never loaded.
    pub fn is_free(&self) -> bool {
        self.buf.is_consumed() || !self.is_loaded()
    }

    pub fn is_drained(&self) -> bool {
        self.drained
    }

    pub fn sample_rate(&self) -> u32 {
        self.sample_rate
    }

    pub fn channels(&self) -> u32 {
        self.channels
    }

    /// Samples the engine may feed to the resampler right now. The
    /// symphonia holdback (encoder padding) is never offered.
    pub fn available(&self) -> usize {
        let holdback = match &self.decoder {
            Decoder::Symphonia { holdback, .. } => *holdback,
            _ => 0,
        };
        self.pending_l.len().saturating_sub(holdback)
    }

    /// Drain up to `n` offered samples from the pending queue into the
    /// caller's per-channel buffers. Caller must not ask for more than
    /// `available()`.
    pub fn take(&mut self, n: usize, out_l: &mut [f32], out_r: &mut [f32]) {
        debug_assert!(n <= self.available());
        let n = n.min(self.available());
        out_l[..n].copy_from_slice(&self.pending_l[..n]);
        out_r[..n].copy_from_slice(&self.pending_r[..n]);
        self.pending_l.drain(..n);
        self.pending_r.drain(..n);
    }

    /// Discard the held-back padding (called once the slot drains).
    fn drop_holdback(&mut self) {
        self.pending_l.clear();
        self.pending_r.clear();
    }

    /// Reset for a fresh track (decoder dropped, queues cleared). The
    /// shared buffer is reset separately by the engine via `reset_slot`.
    /// The buffer's bytes are NOT touched here, so a loop restart can
    /// re-decode the same bytes from the top.
    pub fn reset(&mut self) {
        self.decoder = Decoder::Unprobed;
        self.pending_l.clear();
        self.pending_r.clear();
        self.drained = false;
        self.loaded = false;
        self.sample_rate = 0;
        self.channels = 0;
        self.seen_generation = self.buf.generation();
    }

    /// Mark the slot consumed after promotion: the track played out. The
    /// decoder is dropped and the buffer is flagged so the next push
    /// resets it for a new track (see `SharedBuf::mark_consumed` /
    /// `sync`). The buffer's bytes are left in place until that push.
    pub fn mark_consumed(&mut self) {
        self.decoder = Decoder::Unprobed;
        self.pending_l.clear();
        self.pending_r.clear();
        self.drained = true;
        self.loaded = false;
        self.sample_rate = 0;
        self.channels = 0;
        self.buf.mark_consumed();
    }

    /// Decode more audio into the pending queue.
    ///
    /// The decode step is computed first (borrowing only the decoder
    /// fields), then the slot state is updated afterwards — the two phases
    /// never hold overlapping borrows.
    pub fn decode_more(&mut self) -> Result<SlotOutcome, String> {
        if self.drained {
            return Ok(SlotOutcome::Drained);
        }
        if let Decoder::Failed(e) = &self.decoder {
            return Err(e.clone());
        }
        if matches!(self.decoder, Decoder::Unprobed) {
            self.probe()?;
        }

        enum Step {
            Produced,
            NeedMoreInput,
            Drained,
            Failed(String),
        }

        let step = match &mut self.decoder {
            Decoder::Symphonia {
                format,
                track_id,
                decoder,
                sample_buf,
                spec,
                skip_remaining,
                packets_decoded,
                ..
            } => {
                let track_id = *track_id;
                match format.next_packet() {
                    Err(e) => {
                        if *packets_decoded > 0 {
                            Step::Drained
                        } else {
                            Step::Failed(format!("decode failed before first packet: {e}"))
                        }
                    }
                    Ok(packet) => {
                        if packet.track_id() != track_id {
                            Step::Produced // skip non-audio packet
                        } else {
                            match decoder.decode(&packet) {
                                Ok(decoded) => {
                                    *packets_decoded += 1;
                                    append_decoded(
                                        &decoded,
                                        sample_buf,
                                        spec,
                                        skip_remaining,
                                        &mut self.pending_l,
                                        &mut self.pending_r,
                                    );
                                    Step::Produced
                                }
                                Err(e) => {
                                    if *packets_decoded > 0 {
                                        Step::Drained
                                    } else {
                                        Step::Failed(format!("decode failed: {e}"))
                                    }
                                }
                            }
                        }
                    }
                }
            }
            Decoder::Opus(ogg) => match ogg.decode_more() {
                Ok(OggOutcome::Produced) => {
                    ogg.take_staged(&mut self.pending_l, &mut self.pending_r);
                    Step::Produced
                }
                Ok(OggOutcome::NeedMoreInput) => Step::NeedMoreInput,
                Ok(OggOutcome::Drained) => {
                    ogg.take_staged(&mut self.pending_l, &mut self.pending_r);
                    Step::Drained
                }
                Err(e) => Step::Failed(e),
            },
            Decoder::Unprobed | Decoder::Failed(_) => unreachable!(),
        };

        match step {
            Step::Produced => Ok(SlotOutcome::Produced),
            Step::NeedMoreInput => Ok(SlotOutcome::NeedMoreInput),
            Step::Drained => {
                self.drained = true;
                self.drop_holdback();
                Ok(SlotOutcome::Drained)
            }
            Step::Failed(msg) => {
                self.decoder = Decoder::Failed(msg.clone());
                Err(msg)
            }
        }
    }

    /// Probe the buffered bytes and build the decoder. Blocks inside the
    /// media-source read until enough bytes arrive or input finishes.
    /// Returns Ok on success; on failure the slot is marked Failed.
    fn probe(&mut self) -> Result<(), String> {
        // 1. Ogg-Opus sniff first (symphonia cannot decode Opus at all).
        let mut ogg = OggOpusDecoder::new(Arc::clone(&self.buf));
        match ogg.detect() {
            DetectOutcome::Opus => {
                self.channels = ogg.channels();
                self.sample_rate = 48000;
                self.decoder = Decoder::Opus(ogg);
                self.loaded = true;
                return Ok(());
            }
            DetectOutcome::NotOpus => {
                // Fall through to symphonia.
            }
        }

        // 2. Symphonia probe (blocks in BufReader until data/finished).
        let reader = BufReader::new(Arc::clone(&self.buf));
        let source = MediaSourceStream::new(Box::new(reader), Default::default());
        let probed = match symphonia::default::get_probe().format(
            &Hint::new(),
            source,
            &FormatOptions::default(),
            &MetadataOptions::default(),
        ) {
            Ok(p) => p,
            Err(e) => {
                let msg = format!("probe failed (unsupported or truncated input): {e}");
                self.decoder = Decoder::Failed(msg.clone());
                return Err(msg);
            }
        };
        let format = probed.format;
        let track = match format.default_track() {
            Some(t) => t,
            None => {
                let msg = "probe found no audio track".to_string();
                self.decoder = Decoder::Failed(msg.clone());
                return Err(msg);
            }
        };
        let params = track.codec_params.clone();
        let track_id = track.id;
        let delay = params.delay.unwrap_or(0);
        let padding = params.padding.unwrap_or(0) as usize;
        self.sample_rate = params.sample_rate.unwrap_or(0);
        self.channels = params.channels.map(|c| c.count() as u32).unwrap_or(0);
        if self.sample_rate == 0 {
            let msg = "probe found no sample rate".to_string();
            self.decoder = Decoder::Failed(msg.clone());
            return Err(msg);
        }
        let decoder =
            match symphonia::default::get_codecs().make(&params, &DecoderOptions::default()) {
                Ok(d) => d,
                Err(e) => {
                    let msg = format!("no decoder for codec {:?}: {e}", params.codec);
                    self.decoder = Decoder::Failed(msg.clone());
                    return Err(msg);
                }
            };
        let spec = SignalSpec::new(
            self.sample_rate,
            params
                .channels
                .unwrap_or(symphonia::core::audio::Channels::FRONT_LEFT | symphonia::core::audio::Channels::FRONT_RIGHT),
        );
        let capacity = params.max_frames_per_packet.unwrap_or(4096).max(1024);
        let sample_buf = SampleBuffer::<f32>::new(capacity, spec);
        self.decoder = Decoder::Symphonia {
            format,
            track_id,
            decoder,
            sample_buf,
            spec,
            skip_remaining: delay,
            holdback: padding,
            packets_decoded: 0,
        };
        self.loaded = true;
        Ok(())
    }
}

/// Append one decoded symphonia buffer to the pending queues, converting to
/// stereo and skipping encoder-delay samples. Free function (not a method)
/// so the decode step can borrow the decoder fields and the pending queues
/// disjointly.
#[allow(clippy::too_many_arguments)]
fn append_decoded(
    decoded: &AudioBufferRef,
    sample_buf: &mut SampleBuffer<f32>,
    spec: &mut SignalSpec,
    skip_remaining: &mut u32,
    pending_l: &mut Vec<f32>,
    pending_r: &mut Vec<f32>,
) {
    let frames = decoded.frames();
    let new_spec = *decoded.spec();
    let channels = new_spec.channels.count();
    // Capacity units: SampleBuffer::capacity() is in SAMPLES, while
    // AudioBufferRef::capacity() is in FRAMES. A packet needs
    // frames * channels samples (ffmpeg FLAC blocks are 4608 frames, which
    // exceeds max_frames_per_packet's 4096).
    if new_spec != *spec || sample_buf.capacity() < frames * channels {
        *spec = new_spec;
        let cap_frames = (decoded.capacity()).max(frames);
        *sample_buf = SampleBuffer::<f32>::new(cap_frames as u64, new_spec);
    }
    sample_buf.copy_interleaved_ref(decoded.clone());
    let interleaved = &sample_buf.samples()[..frames * channels];
    let mut start = 0usize;
    if *skip_remaining > 0 {
        let drop = (*skip_remaining as usize).min(frames);
        start = drop;
        *skip_remaining -= drop as u32;
    }
    for i in start..frames {
        match channels {
            1 => {
                let s = interleaved[i];
                pending_l.push(s);
                pending_r.push(s);
            }
            2 => {
                pending_l.push(interleaved[i * 2]);
                pending_r.push(interleaved[i * 2 + 1]);
            }
            _ => {
                // Multichannel: front-left / front-right. (Proper downmix
                // is out of scope; L/R is what a music bot plays.)
                pending_l.push(interleaved[i * channels]);
                pending_r.push(interleaved[i * channels + 1]);
            }
        }
    }
}
