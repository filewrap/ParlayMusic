"""IPC-4 download progress (UI_INVENTORY.md §29).

A thread-safe registry of live download runs. One run per download
request, keyed by a 12-hex ``run_id`` (Navidrome style, cf. §1
``pl:dl:cancel:<run_id>``). The core learns the run_id from the
download response (the ``run_id`` field) and polls
``GET /v1/download/progress?run_id=`` while the download runs.

Every number in a snapshot comes from the real downloader:
- yt-dlp: `--progress --newline` lines parsed by
  ``ytdlp.parse_ytdlp_progress_line`` (percentage / bytes / speed /
  ETA straight off yt-dlp's own output);
- Soulseek: slskd's ``wait_for_files`` ``progress_cb`` (percent,
  state, speed, bytes transferred, size) and ``state_cb``;
- other providers: phase transitions only (searching -> downloading
  -> done/failed) — percentage stays 0 until the file lands, which
  is honest about the granularity available, never an estimate.

A run is kept after it finishes (done/failed) so a late poll still
answers; the registry caps at 200 finished runs, oldest first.
"""
import threading
import time
import uuid

# Phases per §29: searching | downloading | uploading | done | failed.
PHASES = ("searching", "downloading", "uploading", "done", "failed")

_MAX_FINISHED = 200

# Raw PeerTransferError strings carry slskd state names ("transfer
# ended in state 'Completed, TimedOut'") — map the recognisable ones
# to plain language. Ported from music-bot-navidrome
# src/reporting.py::humanize_failure (same mapping, same order).
# Reasons minted by the downloader itself ("nothing found on
# Soulseek", "only lossy copies found (mp3 320 kbps)") pass through
# unchanged.
_FAILURE_MAP = (
    ("queuedremotely", "waiting in the peer queue expired"),
    ("clientside, transfer", "transfer timed out"),
    ("timedout", "peer accepted but never sent the file"),
    ("rejected", "peer rejected the transfer"),
    ("cancelled", "transfer was cancelled"),
    ("errored", "transfer failed on the peer's side"),
    ("refused enqueue", "peer refused the download request"),
    ("could not locate downloaded file",
     "transfer finished but the file never appeared"),
    ("already failed this album",
     "every matching peer had already failed on this album"),
    ("no peer returned a usable file", "all matching peers failed"),
)


def humanize_failure(raw):
    """Map a raw slskd / downloader failure string to plain language.

    Unknown strings pass through unchanged; empty input becomes
    "unknown error" (never a blank reason on the checklist).
    """
    low = (raw or "").lower()
    for needle, text in _FAILURE_MAP:
        if needle in low:
            return text
    return raw or "unknown error"


def new_run_id():
    """12-hex run id (Navidrome style, cf. §1 pl:dl:cancel:<run_id>)."""
    return uuid.uuid4().hex[:12]


class DownloadRun:
    """One download's live state.

    Thread-safe: every mutation goes through a method that takes the
    run's own lock, because the downloader callbacks (yt-dlp reader
    thread, slskd poll loop) update the run while the HTTP thread
    snapshots it. Direct attribute writes are for __init__ only.
    """

    def __init__(self, run_id, kind, title, total_tracks=1):
        self.run_id = run_id
        self.kind = kind            # youtube | soulseek | navidrome | ...
        self.title = title
        self.started_at = time.monotonic()
        self.phase = "searching"
        self.percentage = 0.0
        self.done_bytes = 0
        self.total_bytes = None
        self.speed_bytes_per_sec = 0
        self.eta_sec = None
        # Per-track checklist (navidrome album downloads): one entry
        # per track, {"title", "state", "pct", "peer", "reason"}.
        # States: pending | searching | queued | downloading | done |
        # failed. The checklist collapses above 28 tracks in the core
        # (ref reporting.CHECKLIST_MAX) — the engine still records all.
        self.per_track = []
        self.lossy_available = False
        self.lossy_summary = None
        self.error = None           # humanised failure reason (phase failed)
        self.finished = False
        self._lock = threading.Lock()

    def elapsed_sec(self):
        return int(time.monotonic() - self.started_at)

    def snapshot(self):
        """The IPC-4 download.progress response for this run."""
        with self._lock:
            return {
                "run_id": self.run_id,
                "percentage": round(self.percentage, 1),
                "done_bytes": int(self.done_bytes),
                "total_bytes": self.total_bytes,
                "speed_bytes_per_sec": int(self.speed_bytes_per_sec),
                "eta_sec": self.eta_sec,
                "elapsed_sec": self.elapsed_sec(),
                "phase": self.phase,
                "per_track": [dict(t) for t in self.per_track],
                "lossy_available": self.lossy_available,
                "lossy_summary": self.lossy_summary,
            }

    def update(self, **fields):
        """Set top-level progress fields (percentage, done_bytes,
        total_bytes, speed_bytes_per_sec, eta_sec, phase,
        lossy_available, lossy_summary, error). Unknown keys are
        ignored so a typo in a callback can never corrupt a run."""
        with self._lock:
            for key, value in fields.items():
                if hasattr(self, key) and not key.startswith("_"):
                    setattr(self, key, value)

    def set_titles(self, titles):
        """(Re)build the per-track checklist from titles, preserving
        any entries already updated (by position). Used by providers
        that learn the track list after the server created the run."""
        with self._lock:
            old = self.per_track
            self.per_track = [
                dict(old[i]) if i < len(old) else
                {"title": t, "state": "pending", "pct": 0.0,
                 "peer": None, "reason": None}
                for i, t in enumerate(titles)
            ]

    def set_track(self, index, state=None, pct=None, peer=None,
                  reason=None):
        """Update one checklist entry by index (0-based). `reason` is
        stored humanised. No-op when the checklist has no such index
        (the provider has not built it yet)."""
        with self._lock:
            if index >= len(self.per_track):
                return
            entry = self.per_track[index]
            if state is not None:
                entry["state"] = state
            if pct is not None:
                entry["pct"] = round(float(pct), 1)
            if peer is not None:
                entry["peer"] = peer
            if reason is not None:
                entry["reason"] = humanize_failure(reason)

    def finish(self, phase, error=None):
        """Mark the run done/failed; on done the percentage is 100.
        An error always means failed, even if phase "done" was passed
        (a caller must not finish a run done with an error attached)."""
        with self._lock:
            self.phase = phase
            self.finished = True
            if phase == "done":
                self.percentage = 100.0
            if error:
                self.error = humanize_failure(error)
                self.phase = "failed"


class ProgressRegistry:
    """Thread-safe store of DownloadRuns, keyed by run_id."""

    def __init__(self):
        self._lock = threading.Lock()
        self._runs = {}
        self._finished_order = []

    def new_run(self, kind, title, total_tracks=1, per_track_titles=None):
        run = DownloadRun(new_run_id(), kind, title, total_tracks)
        if per_track_titles:
            run.set_titles(per_track_titles)
        with self._lock:
            self._runs[run.run_id] = run
        return run

    def get(self, run_id):
        with self._lock:
            return self._runs.get(run_id)

    def snapshot(self, run_id):
        run = self.get(run_id)
        return run.snapshot() if run else None

    def set_titles(self, run_id, titles):
        run = self.get(run_id)
        if run is not None:
            run.set_titles(titles)

    def update(self, run_id, **fields):
        run = self.get(run_id)
        if run is not None:
            run.update(**fields)

    def set_track(self, run_id, index, state=None, pct=None, peer=None,
                  reason=None):
        run = self.get(run_id)
        if run is not None:
            run.set_track(index, state=state, pct=pct, peer=peer,
                          reason=reason)

    def finish(self, run_id, phase, error=None):
        """Mark a run done/failed and cap the finished-run history at
        200 (oldest first); live runs are never evicted."""
        run = self.get(run_id)
        if run is None:
            return
        run.finish(phase, error=error)
        with self._lock:
            self._finished_order.append(run_id)
            while len(self._finished_order) > _MAX_FINISHED:
                old = self._finished_order.pop(0)
                old_run = self._runs.get(old)
                if old_run is not None and old_run.finished:
                    del self._runs[old]


# The process-wide registry. The HTTP layer and the providers share
# this instance; tests construct their own ProgressRegistry instead.
REGISTRY = ProgressRegistry()
