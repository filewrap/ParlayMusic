//! Minimal incremental Ogg demuxer + libopus decode for `.opus` / Ogg-Opus.
//!
//! Why this exists: symphonia 0.5 has no Opus codec, and the locked spec
//! requires Opus decode. The demuxer is deliberately small — Ogg pages are
//! a simple framing (capture pattern, granule, segment table) — and Opus
//! packets decode through the system libopus (see `opus_ffi.rs`).
//!
//! Gapless trimming (exact, per RFC 7845):
//! * **Start:** `OpusHead.pre_skip` samples are dropped from the front of
//!   the decoded stream.
//! * **End:** the granule position of the page a packet completes on is the
//!   presented-sample number of that packet's last sample (the spec already
//!   excludes pre-skip from granule positions). A packet is truncated to
//!   `granule - presented_emitted_before`, so the stream ends on the exact
//!   sample — no padding samples leak into the resampler.
//! * A page whose granule is -1 completes no packet, so no trim is due
//!   from it; the trim is always taken from the completing page's granule.
//!
//! Only mono/stereo Opus is supported (music content); a multichannel
//! OpusHead is a clean error, not a silent downmix.

use crate::opus_ffi::{OpusDecoder, OPUS_MAX_FRAME_SAMPLES};
use crate::source::SharedBuf;
use std::sync::Arc;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OggOutcome {
    /// At least one packet was decoded into the staged buffers.
    Produced,
    /// No complete packet available yet and input is not finished.
    NeedMoreInput,
    /// Input finished and all complete packets decoded.
    Drained,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DetectOutcome {
    Opus,
    NotOpus,
}

/// One Ogg page header (27 bytes) + segment table.
struct PageHeader {
    granule: i64,
    segments: Vec<u8>,
    total_len: usize, // header + segment table + payload
}

fn parse_page_header(buf: &[u8]) -> Option<PageHeader> {
    if buf.len() < 27 || &buf[0..4] != b"OggS" {
        return None;
    }
    let granule = i64::from_le_bytes(buf[6..14].try_into().ok()?);
    let n_segments = buf[26] as usize;
    if buf.len() < 27 + n_segments {
        return None;
    }
    let segments = buf[27..27 + n_segments].to_vec();
    let payload_len: usize = segments.iter().map(|&s| s as usize).sum();
    Some(PageHeader {
        granule,
        segments,
        total_len: 27 + n_segments + payload_len,
    })
}

pub struct OggOpusDecoder {
    buf: Arc<SharedBuf>,
    /// Bytes already moved from the shared buffer into `pending`.
    cursor: u64,
    /// Unparsed bytes (partial page tail).
    pending: Vec<u8>,
    /// Packet being reassembled across pages.
    packet: Vec<u8>,
    /// Packet counter: 0 = OpusHead, 1 = OpusTags, 2+ = audio.
    packet_no: u64,
    decoder: Option<OpusDecoder>,
    channels: u32,
    pre_skip: u32,
    /// Presented samples emitted so far (post pre-skip, post end-trim).
    emitted: u64,
    /// Raw decoded samples still inside the pre-skip region.
    preskip_left: u32,
    /// True once input is finished and every complete packet is decoded.
    drained: bool,
    /// Scratch for one decoded Opus packet (interleaved f32).
    pcm_scratch: Vec<f32>,
    /// Staged decoded stereo, drained by the slot into its pending queue.
    staged_l: Vec<f32>,
    staged_r: Vec<f32>,
}

impl OggOpusDecoder {
    pub fn new(buf: Arc<SharedBuf>) -> Self {
        OggOpusDecoder {
            buf,
            cursor: 0,
            pending: Vec::new(),
            packet: Vec::new(),
            packet_no: 0,
            decoder: None,
            channels: 0,
            pre_skip: 0,
            emitted: 0,
            preskip_left: 0,
            drained: false,
            pcm_scratch: Vec::new(),
            staged_l: Vec::new(),
            staged_r: Vec::new(),
        }
    }

    /// Decide whether this buffer holds Ogg-Opus. Blocks until the first
    /// packet is complete or input finishes. Call once, before the
    /// symphonia probe path. On `Opus`, the first page is left unparsed so
    /// the normal decode pass handles it exactly once (the OpusHead packet
    /// is re-parsed; `handle_packet` for packet 0 is idempotent).
    pub fn detect(&mut self) -> DetectOutcome {
        loop {
            let (bytes, finished, shutdown, _) = self.buf.snapshot_from(self.cursor);
            self.cursor += bytes.len() as u64;
            self.pending.extend_from_slice(&bytes);
            if shutdown {
                return DetectOutcome::NotOpus;
            }
            match self.parse_pages(true) {
                Ok(Some(is_opus)) => {
                    return if is_opus {
                        DetectOutcome::Opus
                    } else {
                        DetectOutcome::NotOpus
                    }
                }
                Ok(None) => {
                    if finished {
                        return DetectOutcome::NotOpus;
                    }
                    // Need more bytes for the first packet.
                    self.buf.wait_for_bytes((self.cursor as usize) + 4096);
                    if self.buf.is_finished() {
                        // Loop around once more with the final bytes; the
                        // next iteration returns NotOpus if still incomplete.
                        continue;
                    }
                }
                Err(_) => return DetectOutcome::NotOpus,
            }
        }
    }

    /// Parse complete pages from `pending`.
    ///
    /// When `stop_after_first_packet` is set, returns `Ok(Some(is_opus))`
    /// as soon as the first packet completes (and leaves the page unparsed
    /// so the decode pass re-handles it), or `Ok(None)` if no packet
    /// completed yet. Otherwise decodes audio packets into the staged
    /// buffers and returns `Ok(None)`.
    fn parse_pages(&mut self, stop_after_first_packet: bool) -> Result<Option<bool>, String> {
        loop {
            let hdr = match parse_page_header(&self.pending) {
                Some(h) => h,
                None => {
                    if self.pending.len() >= 4 && &self.pending[0..4] != b"OggS" {
                        return Err("not an Ogg stream".to_string());
                    }
                    return Ok(None); // partial header: wait for more bytes
                }
            };
            if self.pending.len() < hdr.total_len {
                return Ok(None); // partial page: wait for more bytes
            }
            let payload_start = 27 + hdr.segments.len();
            // Copy the payload out: handling a packet mutates self, which
            // cannot borrow from self.pending at the same time.
            let payload: Vec<u8> =
                self.pending[payload_start..hdr.total_len].to_vec();
            let mut off = 0usize;
            for &seg_len in &hdr.segments {
                let seg = &payload[off..off + seg_len as usize];
                off += seg_len as usize;
                self.packet.extend_from_slice(seg);
                if (seg_len as usize) < 255 {
                    // Packet completes on this page.
                    let completed = std::mem::take(&mut self.packet);
                    let granule = hdr.granule;
                    if stop_after_first_packet {
                        let is_opus = completed.starts_with(b"OpusHead");
                        // Restore the packet; the decode pass re-parses
                        // this page from the top (pending is not drained).
                        self.packet = completed;
                        return Ok(Some(is_opus));
                    }
                    self.handle_packet(&completed, granule)?;
                }
            }
            self.pending.drain(..hdr.total_len);
            if stop_after_first_packet {
                return Ok(None);
            }
        }
    }

    fn handle_packet(&mut self, packet: &[u8], page_granule: i64) -> Result<(), String> {
        match self.packet_no {
            0 => {
                if !packet.starts_with(b"OpusHead") || packet.len() < 19 {
                    return Err("first Ogg packet is not a valid OpusHead".to_string());
                }
                let channels = packet[9] as u32;
                if channels == 0 || channels > 2 {
                    return Err(format!(
                        "unsupported Opus channel count {channels} (mono/stereo only)"
                    ));
                }
                let pre_skip = u16::from_le_bytes([packet[10], packet[11]]) as u32;
                self.channels = channels;
                self.pre_skip = pre_skip;
                self.preskip_left = pre_skip;
                self.decoder = Some(OpusDecoder::new(48000, channels)?);
            }
            1 => {
                // OpusTags: metadata, skipped.
            }
            _ => {
                // Decode first (borrows decoder + scratch), then stage the
                // kept samples (borrows staged buffers) — kept in separate
                // scopes so the borrows never overlap.
                let (n, start, keep) = {
                    let dec = self.decoder.as_mut().ok_or("Opus packet before OpusHead")?;
                    let frame_cap = OPUS_MAX_FRAME_SAMPLES * self.channels as usize;
                    self.pcm_scratch.resize(frame_cap, 0.0);
                    let n = dec.decode(packet, &mut self.pcm_scratch, OPUS_MAX_FRAME_SAMPLES)?;
                    let mut start = 0usize;
                    if self.preskip_left > 0 {
                        let drop = (self.preskip_left as usize).min(n);
                        start += drop;
                        self.preskip_left -= drop as u32;
                    }
                    let mut keep = n.saturating_sub(start);
                    if page_granule >= 0 {
                        let valid_here = (page_granule as u64).saturating_sub(self.emitted);
                        keep = keep.min(valid_here as usize);
                    }
                    (n, start, keep)
                };
                let _ = n;
                let ch = self.channels as usize;
                for i in start..start + keep {
                    if ch == 2 {
                        let l = self.pcm_scratch[i * 2];
                        let r = self.pcm_scratch[i * 2 + 1];
                        self.staged_l.push(l);
                        self.staged_r.push(r);
                    } else {
                        let s = self.pcm_scratch[i];
                        self.staged_l.push(s);
                        self.staged_r.push(s);
                    }
                }
                self.emitted += keep as u64;
            }
        }
        self.packet_no += 1;
        Ok(())
    }

    /// Decode as much as possible into the staged buffers.
    pub fn decode_more(&mut self) -> Result<OggOutcome, String> {
        if self.drained {
            return Ok(OggOutcome::Drained);
        }
        let before = self.staged_l.len();
        let (bytes, finished, shutdown, _) = self.buf.snapshot_from(self.cursor);
        if shutdown {
            self.drained = true;
            return Ok(OggOutcome::Drained);
        }
        self.cursor += bytes.len() as u64;
        self.pending.extend_from_slice(&bytes);
        self.parse_pages(false)?;
        if self.staged_l.len() > before {
            return Ok(OggOutcome::Produced);
        }
        if finished {
            // No complete packet left and input is done; a partial trailing
            // page is discarded (truncated tail ends the stream, not an error).
            self.drained = true;
            return Ok(OggOutcome::Drained);
        }
        Ok(OggOutcome::NeedMoreInput)
    }

    /// Move staged samples out (the slot drains these into its pending queue).
    pub fn take_staged(&mut self, out_l: &mut Vec<f32>, out_r: &mut Vec<f32>) {
        out_l.append(&mut self.staged_l);
        out_r.append(&mut self.staged_r);
    }

    pub fn channels(&self) -> u32 {
        self.channels
    }
}
