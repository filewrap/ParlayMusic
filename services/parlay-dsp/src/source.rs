//! Shared growable byte buffer fed by Go (`push`) and consumed by the
//! decoders (`pull` path). This is the only cross-thread state in the
//! engine that is touched *without* the engine mutex, which is what makes
//! the push/pull split deadlock-free:
//!
//! * `push` clones the slot's `Arc<SharedBuf>` (lock-free) and appends under
//!   the buffer's own mutex. It never takes the engine mutex.
//! * `pull` holds the engine mutex while decoding, and the decoder's reads
//!   block on this buffer's condvar until bytes arrive, input finishes, or
//!   the engine is destroyed. Because the buffer lock is separate from the
//!   engine lock, a blocked pull can never starve a push.
//!
//! Contract with Go: push and pull must run on different goroutines. A pull
//! may block inside a decoder read until more bytes are pushed or
//! `finish_input` is called. `destroy` wakes all blocked readers.

use std::io::{self, Read, Seek, SeekFrom};
use std::sync::{Arc, Condvar, Mutex};

#[derive(Debug, Default)]
struct BufState {
    /// All bytes ever pushed for the current track. Retained in full so a
    /// loop restart can re-decode from byte 0 without Go re-pushing.
    data: Vec<u8>,
    /// Go called `finish_input`: no more bytes will arrive for this track.
    finished: bool,
    /// Engine destroyed: readers must EOF immediately.
    shutdown: bool,
    /// Bumped on every reset so a stale reader can tell its data is gone.
    generation: u64,
    /// Set when the slot was consumed (track played out). The next push
    /// starts a fresh track: buffer is cleared first.
    needs_reset: bool,
}

/// A growable byte buffer shared between Go's push side and the decode side.
#[derive(Debug)]
pub struct SharedBuf {
    state: Mutex<BufState>,
    cond: Condvar,
}

impl Default for SharedBuf {
    fn default() -> Self {
        SharedBuf {
            state: Mutex::new(BufState::default()),
            cond: Condvar::new(),
        }
    }
}

impl SharedBuf {
    pub fn new() -> Arc<Self> {
        Arc::new(SharedBuf::default())
    }

    /// Append bytes. If the slot was consumed, the buffer is cleared first
    /// (a new track starts). Wakes blocked readers.
    pub fn push(&self, data: &[u8]) {
        let mut st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        if st.needs_reset {
            st.data.clear();
            st.finished = false;
            st.generation = st.generation.wrapping_add(1);
            st.needs_reset = false;
        }
        st.data.extend_from_slice(data);
        drop(st);
        self.cond.notify_all();
    }

    /// Mark input complete: no more bytes will arrive for this track.
    pub fn finish(&self) {
        let mut st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        st.finished = true;
        drop(st);
        self.cond.notify_all();
    }

    /// Mark the slot consumed. The next push resets the buffer. The decoder
    /// state for the old track is dropped by the engine separately.
    pub fn mark_consumed(&self) {
        let mut st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        st.needs_reset = true;
        drop(st);
        self.cond.notify_all();
    }

    /// Wake all readers with EOF. Called from `destroy`.
    pub fn shutdown(&self) {
        let mut st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        st.shutdown = true;
        drop(st);
        self.cond.notify_all();
    }

    /// Clear the buffer immediately (explicit `reset_slot` from Go).
    pub fn reset(&self) {
        let mut st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        st.data.clear();
        st.finished = false;
        st.generation = st.generation.wrapping_add(1);
        st.needs_reset = false;
        drop(st);
        self.cond.notify_all();
    }

    /// Number of bytes currently buffered.
    pub fn len(&self) -> usize {
        self.state.lock().unwrap_or_else(|e| e.into_inner()).data.len()
    }

    /// Current generation (bumped whenever the buffer is reset for a new
    /// track). Slots watch this to know their decoder state is stale.
    pub fn generation(&self) -> u64 {
        self.state.lock().unwrap_or_else(|e| e.into_inner()).generation
    }

    /// True when the slot was consumed and the buffer is waiting for the
    /// next track's first push (which resets it).
    pub fn is_consumed(&self) -> bool {
        self.state.lock().unwrap_or_else(|e| e.into_inner()).needs_reset
    }

    pub fn is_finished(&self) -> bool {
        self.state.lock().unwrap_or_else(|e| e.into_inner()).finished
    }

    /// Block until at least `min_bytes` are buffered, or input is finished /
    /// shut down. Returns true if the requested bytes are available.
    pub fn wait_for_bytes(&self, min_bytes: usize) -> bool {
        let mut st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        loop {
            if st.data.len() >= min_bytes || st.finished || st.shutdown {
                return st.data.len() >= min_bytes;
            }
            st = self.cond.wait(st).unwrap_or_else(|e| e.into_inner());
        }
    }

    /// Copy bytes starting at `pos` without blocking. Returns
    /// `(bytes, finished, shutdown, generation)`.
    pub fn snapshot_from(&self, pos: u64) -> (Vec<u8>, bool, bool, u64) {
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        let start = (pos as usize).min(st.data.len());
        (
            st.data[start..].to_vec(),
            st.finished,
            st.shutdown,
            st.generation,
        )
    }

    /// Blocking read used by the symphonia `MediaSource` below: waits until
    /// at least one byte is available at `pos`, or input finished/shut down.
    /// Returns the number of bytes copied (0 = EOF).
    fn read_blocking(&self, pos: u64, out: &mut [u8]) -> io::Result<usize> {
        let mut st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        loop {
            if st.shutdown {
                return Ok(0);
            }
            let avail = st.data.len().saturating_sub(pos as usize);
            if avail > 0 {
                let n = avail.min(out.len());
                let start = pos as usize;
                out[..n].copy_from_slice(&st.data[start..start + n]);
                return Ok(n);
            }
            if st.finished {
                return Ok(0);
            }
            st = self.cond.wait(st).unwrap_or_else(|e| e.into_inner());
        }
    }

    /// Block until at least `target` bytes are buffered, or input is
    /// finished / shut down. Returns the bytes available.
    fn wait_for_len(&self, target: usize) -> usize {
        let mut st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        loop {
            if st.data.len() >= target || st.finished || st.shutdown {
                return st.data.len();
            }
            st = self.cond.wait(st).unwrap_or_else(|e| e.into_inner());
        }
    }
}

/// A reader over a [`SharedBuf`] that implements symphonia's `MediaSource`
/// (Read + Seek).
///
/// The buffer retains every byte, so seeking is real random access over the
/// bytes buffered so far. A seek past the buffered prefix blocks until the
/// bytes arrive or input finishes — this is what lets symphonia's isomp4
/// reader find a `moov` atom at the END of a non-faststart MP4 (it seeks
/// there once the file is fully pushed). `byte_len` is only known once
/// input finishes.
pub struct BufReader {
    buf: Arc<SharedBuf>,
    pos: u64,
    generation: u64,
}

impl BufReader {
    pub fn new(buf: Arc<SharedBuf>) -> Self {
        let generation = buf.state.lock().unwrap_or_else(|e| e.into_inner()).generation;
        BufReader {
            buf,
            pos: 0,
            generation,
        }
    }

    fn seek_to(&mut self, target: u64) -> io::Result<u64> {
        // Block until the target is buffered, or input is done.
        let have = self.buf.wait_for_len(target as usize);
        if (target as usize) <= have {
            self.pos = target;
            Ok(target)
        } else {
            Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "seek beyond end of input",
            ))
        }
    }
}

impl Read for BufReader {
    fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
        // A generation bump means the buffer was reset under us (a new track
        // was pushed into this slot). Treat it as EOF: the old decoder ends.
        if self.buf.state.lock().unwrap_or_else(|e| e.into_inner()).generation != self.generation {
            return Ok(0);
        }
        let n = self.buf.read_blocking(self.pos, out)?;
        self.pos += n as u64;
        Ok(n)
    }
}

impl Seek for BufReader {
    fn seek(&mut self, pos: SeekFrom) -> io::Result<u64> {
        match pos {
            SeekFrom::Start(n) => self.seek_to(n),
            SeekFrom::Current(0) => Ok(self.pos),
            SeekFrom::Current(off) => {
                let target = self.pos as i64 + off;
                if target < 0 {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidInput,
                        "seek before start of input",
                    ));
                }
                self.seek_to(target as u64)
            }
            SeekFrom::End(off) => {
                // End-relative seek needs the total length: wait until
                // input finishes (wait_for_len(usize::MAX) only returns
                // early on finished/shutdown).
                let total = self.buf.wait_for_len(usize::MAX);
                let target = total as i64 + off;
                if target < 0 || target as usize > total {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidInput,
                        "seek beyond end of input",
                    ));
                }
                self.pos = target as u64;
                Ok(self.pos)
            }
        }
    }
}

impl symphonia::core::io::MediaSource for BufReader {
    fn is_seekable(&self) -> bool {
        true
    }

    fn byte_len(&self) -> Option<u64> {
        if self.buf.is_finished() {
            Some(self.buf.len() as u64)
        } else {
            None
        }
    }
}
