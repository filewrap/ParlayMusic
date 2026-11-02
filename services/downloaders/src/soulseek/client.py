"""Soulseek client — Parlay's port of music-bot-navidrome's soulseek package.

The ref is asyncio + `slskd-api` (requests). Parlay's downloaders
service is a sync ThreadingHTTPServer with a stdlib-only HTTP stack,
so this port is sync + urllib and talks to slskd's HTTP API directly
(endpoints verified against slskd-api 0.2.4, not guessed):

  POST   /api/v0/searches                     start a search
  GET    /api/v0/searches/{id}                search state
  PUT    /api/v0/searches/{id}                stop (flushes responses)
  DELETE /api/v0/searches/{id}                delete
  GET    /api/v0/searches/{id}/responses       peer responses
  POST   /api/v0/transfers/downloads/{user}    enqueue [{filename, size}]
  GET    /api/v0/transfers/downloads/          all downloads
  GET    /api/v0/transfers/downloads/{user}    one user's downloads
  GET    /api/v0/transfers/downloads/{user}/{id}/position
  DELETE /api/v0/transfers/downloads/{user}/{id}?remove=
  DELETE /api/v0/transfers/downloads/all/completed
  POST   /api/v0/shares                        start a share rescan

Auth: `X-API-Key: $SLSKD_API_KEY` on every call. Base URL
`$SLSKD_URL` (default http://slskd:5030; `$SLSKD_HOST` is accepted
as an alias, matching the ref's env name).

Search behaviour (kept verbatim from the ref — it is load-bearing):
  1. Searches are globally serialized and paced — the Soulseek server
     silently drops search floods (and hands out 30-minute bans for
     sustained abuse). One search per 10 s
     ($SLSKD_SEARCH_MIN_INTERVAL_SECS).
  2. A search that *couldn't run* raises SearchError /
     SearchThrottledError; an empty return always means the search ran
     and genuinely found nothing.
  3. `state(includeResponses=True)` sometimes returns an empty list
     even when `responseCount > 0`, so we read `search_responses(id)`
     after forcing a flush via `stop()` (PUT). The `Cancelled` flag
     slskd sets in that path is cosmetic — responses are preserved.

Env:
  SLSKD_URL / SLSKD_HOST — slskd base URL (default http://slskd:5030)
  SLSKD_API_KEY          — slskd API key (empty = no auth header)
  SLSKD_API_TIMEOUT_SECS — per-request timeout (default 30)
  SLSKD_SEARCH_MIN_INTERVAL_SECS — search pacing (default 10)
  SLSKD_DOWNLOAD_DIR     — where slskd writes downloads (default:
                           $PARLAY_DL_DIR/.slskd-downloads)
  SLSKD_STALL_SECS       — give up on a started-but-stalled transfer
                           after this many seconds of no bytes (default
                           120; 0 disables)
  SLSKD_DOWNLOAD_TIMEOUT — per-attempt queue+transfer cap (default 900)
"""
import json
import logging
import os
import re
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

logger = logging.getLogger(__name__)


class SearchError(Exception):
    """The search could not be executed (slskd unreachable, API error).
    Distinct from a search that ran and found nothing — callers must not
    report this as 'no match'."""


class SearchThrottledError(SearchError):
    """slskd (or the Soulseek server behind it) is rate-limiting our
    searches and backoff didn't clear it."""


# LOSSLESS_EXTS is FLAC-only because the tagger has no native WAV/AIFF writer
# — accepting them would put WAV bytes inside .flac-named files. ALAC ships in
# .m4a so it's handled through the M4A path. (Kept from the ref; Parlay
# does not retag, but the same extension policy applies to downloads.)
LOSSLESS_EXTS = {"flac"}
LOSSY_EXTS = {"mp3", "aac", "m4a", "ogg", "opus", "wma", "alac", "wav", "aiff"}
AUDIO_EXTS = LOSSLESS_EXTS | LOSSY_EXTS

IMAGE_EXTS = {"jpg", "jpeg", "png", "webp"}

# Username is dropped into ``<download_dir>/<username>/...`` paths, so anything
# outside this character set could escape the download root or waste a fs walk.
_SAFE_USERNAME_RE = re.compile(r"[^A-Za-z0-9._-]")


class SearchResult:
    """A single file from a slskd peer response."""

    def __init__(self, username, filename, size, bit_rate=None, bit_depth=None,
                 sample_rate=None, length=None, has_free_slot=False,
                 upload_speed=0, queue_length=0, score=0.0):
        self.username = username
        self.filename = filename  # full remote path; slskd uses backslashes
        self.size = size
        self.bit_rate = bit_rate
        self.bit_depth = bit_depth
        self.sample_rate = sample_rate
        self.length = length  # duration in seconds
        self.has_free_slot = has_free_slot
        self.upload_speed = upload_speed
        self.queue_length = queue_length
        self.score = score  # filled in by scorer

    @property
    def basename(self):
        if "\\" in self.filename:
            return self.filename.rsplit("\\", 1)[-1]
        return self.filename.rsplit("/", 1)[-1]

    @property
    def directory(self):
        if "\\" in self.filename:
            return self.filename.rsplit("\\", 1)[0]
        return self.filename.rsplit("/", 1)[0] if "/" in self.filename else ""

    @property
    def extension(self):
        b = self.basename
        return b.rsplit(".", 1)[-1].lower() if "." in b else ""

    @property
    def is_lossless(self):
        return self.extension in LOSSLESS_EXTS

    def __repr__(self):
        return "SearchResult(%r, %r, %dB)" % (self.username, self.basename, self.size)


class PeerFolder:
    """A directory on one peer holding a group of audio files (album folder candidate)."""

    def __init__(self, username, directory, files=None, has_free_slot=False,
                 upload_speed=0, queue_length=0, score=0.0):
        self.username = username
        self.directory = directory
        self.files = list(files or [])
        self.has_free_slot = has_free_slot
        self.upload_speed = upload_speed
        self.queue_length = queue_length
        self.score = score

    @property
    def lossless_count(self):
        return sum(1 for f in self.files if f.is_lossless)

    @property
    def total_size(self):
        return sum(f.size for f in self.files)


# --- slskd HTTP ------------------------------------------------------------

def _base_url():
    return (os.environ.get("SLSKD_URL")
            or os.environ.get("SLSKD_HOST")
            or "http://slskd:5030").rstrip("/")


def _timeout():
    try:
        return float(os.environ.get("SLSKD_API_TIMEOUT_SECS", "30"))
    except ValueError:
        return 30.0


def _api_key():
    return os.environ.get("SLSKD_API_KEY") or ""


class SlskdHTTPError(Exception):
    def __init__(self, status, body):
        super().__init__("slskd HTTP %s: %s" % (status, body))
        self.status = status
        self.body = body


def _request(method, path, payload=None):
    """One slskd API call. Returns parsed JSON (or True for empty
    2xx bodies). Raises SlskdHTTPError on HTTP errors and SearchError
    on transport failures — the search layer maps those."""
    url = _base_url() + "/api/v0" + path
    headers = {"Accept": "application/json"}
    key = _api_key()
    if key:
        headers["X-API-Key"] = key
    data = None
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=_timeout()) as resp:
            raw = resp.read()
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace") if exc.fp else ""
        raise SlskdHTTPError(exc.code, body)
    except Exception as exc:
        raise SearchError("slskd request failed (%s %s): %s" % (method, path, exc))
    if not raw:
        return True
    try:
        return json.loads(raw.decode("utf-8"))
    except ValueError:
        return True


def slskd_available():
    """True when slskd answers its application endpoint (used by the
    provider to fail fast with a clear error instead of timing out a
    search). Any HTTP response — even an auth error — counts as
    'the daemon is there'; only transport failure counts as down."""
    try:
        _request("GET", "/application")
        return True
    except SlskdHTTPError:
        return True
    except SearchError:
        return False


# --- shares ------------------------------------------------------------------

def rescan_shares():
    """Kick an immediate full slskd share rescan. 409 (scan already
    running) is success-equivalent. Returns True on success."""
    try:
        _request("POST", "/shares")
        logger.info("slskd share rescan triggered")
        return True
    except SlskdHTTPError as exc:
        if exc.status == 409 or "already" in (exc.body or "").lower():
            return True
        logger.warning("slskd share rescan failed: %s", exc)
        return False
    except SearchError as exc:
        logger.warning("slskd share rescan failed: %s", exc)
        return False


# --- staging cleanup ---------------------------------------------------------

_ORPHAN_GRACE_SECS = 60


def cleanup_orphan_staging(download_dir=None):
    """Remove leftover files in the slskd staging dir that aren't tied to an
    active transfer. Safe to skip if the slskd API is unreachable.
    Skips slskd's own ``.incomplete/`` tree, files with a basename
    matching an active (non-Completed) transfer, and anything modified
    within the last 60 s."""
    if download_dir is None:
        download_dir = slskd_download_dir()
    if not os.path.isdir(download_dir):
        return 0
    try:
        raw = _request("GET", "/transfers/downloads/")
    except Exception as exc:
        logger.warning("Orphan sweep skipped — couldn't query slskd transfers: %s", exc)
        return 0
    active_basenames = set()
    for row in _flatten_downloads(raw):
        state = row.get("state", "")
        if "Completed" in state:
            continue
        fname = row.get("filename", "")
        bn = fname.rsplit("\\", 1)[-1] if "\\" in fname else fname.rsplit("/", 1)[-1]
        if bn:
            active_basenames.add(bn)
    cutoff = time.time() - _ORPHAN_GRACE_SECS
    removed = 0
    for entry in os.scandir(download_dir):
        if entry.name == ".incomplete":
            continue
        if entry.is_dir(follow_symlinks=False):
            for root, _, files in os.walk(entry.path, topdown=False):
                for fname in files:
                    if fname in active_basenames:
                        continue
                    fp = os.path.join(root, fname)
                    try:
                        if os.path.getmtime(fp) >= cutoff:
                            continue
                        os.remove(fp)
                        removed += 1
                    except OSError:
                        pass
                try:
                    os.rmdir(root)
                except OSError:
                    pass
        else:
            if entry.name in active_basenames:
                continue
            try:
                if entry.stat().st_mtime >= cutoff:
                    continue
                os.remove(entry.path)
                removed += 1
            except OSError:
                pass
    if removed:
        logger.info("Orphan staging sweep: removed %d file(s)", removed)
    return removed


def slskd_download_dir(dl_dir=None):
    """Where slskd writes completed downloads. Env override first,
    else $PARLAY_DL_DIR/.slskd-downloads, else ./downloads/."""
    if os.environ.get("SLSKD_DOWNLOAD_DIR"):
        return os.environ["SLSKD_DOWNLOAD_DIR"]
    base = dl_dir or os.environ.get("PARLAY_DL_DIR") or os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "downloads")
    return os.path.join(base, ".slskd-downloads")


# --- search ------------------------------------------------------------------

# The Soulseek server silently drops search floods and bans repeat offenders
# for 30 minutes. slsk-batchdl — the reference batch downloader — caps itself
# at 34 searches per 220s (~1 per 6.5s) for exactly this reason. One search
# per 10s stays under that ceiling with headroom for manual searches from the
# slskd web UI, which share the same server connection but bypass this pacer.
SEARCH_MIN_INTERVAL_SECS = float(os.environ.get("SLSKD_SEARCH_MIN_INTERVAL_SECS", "10"))

# 429 from slskd's own API means search starts piled up locally. The observed
# server-side drop window after a flood is ~90s, so back off in growing steps
# before declaring the search impossible.
_HTTP_429_BACKOFFS_SECS = (30.0, 60.0, 90.0)

# N consecutive all-empty searches are treated as suspected server-side
# throttling (the server drops flooded searches silently instead of erroring):
# cool down once, re-run the query, and only believe the emptiness if the
# re-check is empty too.
_ZERO_BURST_THRESHOLD = 3
_ZERO_BURST_COOLDOWN_SECS = 90.0
_ZERO_VERIFIED_WINDOW_SECS = 300.0

_search_lock = threading.Lock()
_next_search_start = 0.0    # monotonic; no search may start before this
_zero_streak = 0            # consecutive searches that returned no responses
_zero_verified_until = 0.0  # monotonic; while in the window, empties are trusted


def _defer_search(delay_secs):
    """Push the earliest allowed start of the next search further out."""
    global _next_search_start
    _next_search_start = max(_next_search_start, time.monotonic() + delay_secs)


def _start_search(query, timeout_secs, response_limit, on_wait=None):
    """Start a search on slskd, retrying 429s through the backoff ladder.
    Must be called with ``_search_lock`` held. Returns the search state
    dict. ``on_wait`` (optional ``cb(secs)``) fires before any
    noticeable pacing/backoff sleep so a UI can explain the silence."""
    global _next_search_start
    for attempt, next_backoff in enumerate((*_HTTP_429_BACKOFFS_SECS, None)):
        wait = _next_search_start - time.monotonic()
        if wait > 0:
            if on_wait is not None and wait > 2:
                try:
                    on_wait(wait)
                except Exception:
                    pass
            time.sleep(wait)
        try:
            state = _request("POST", "/searches", {
                "id": str(uuid.uuid1()),
                "searchText": query,
                "searchTimeout": timeout_secs * 1000,
                "responseLimit": response_limit,
                "fileLimit": 10000,
                "filterResponses": True,
                "maximumPeerQueueLength": 1000000,
                "minimumPeerUploadSpeed": 0,
                "minimumResponseFileCount": 1,
            })
        except SlskdHTTPError as exc:
            if exc.status != 429:
                raise SearchError("slskd search API error: %s" % exc)
            if next_backoff is None:
                raise SearchThrottledError(
                    "slskd kept rate-limiting searches (429) through "
                    "%d backoff retries" % len(_HTTP_429_BACKOFFS_SECS))
            logger.warning("slskd returned 429 for search %r (attempt %d) — "
                           "backing off %.0fs", query, attempt + 1, next_backoff)
            _defer_search(next_backoff)
            continue
        _defer_search(SEARCH_MIN_INTERVAL_SECS)
        return state
    raise AssertionError("unreachable")  # loop always returns or raises


def search(query, timeout_secs=20, response_limit=200,
           _throttle_probe=False, on_wait=None):
    """Run a slskd search and return the raw peer responses.

    Contract: an empty list means the search *ran* and nobody had a match.
    A search that couldn't run raises ``SearchError`` (``SearchThrottledError``
    when rate-limiting persisted through backoff). When a burst of consecutive
    searches comes back empty, the Soulseek server is probably silently
    dropping our queries — cool down and re-run the query once before
    believing the emptiness (``_throttle_probe`` marks that internal retry).
    """
    global _zero_streak, _zero_verified_until

    responses = _search_once(query, timeout_secs, response_limit, on_wait)
    if responses:
        _zero_streak = 0
        return responses

    _zero_streak += 1
    if _throttle_probe or _zero_streak < _ZERO_BURST_THRESHOLD \
            or time.monotonic() < _zero_verified_until:
        return responses

    logger.warning(
        "%d consecutive empty searches — suspecting server-side search "
        "throttling; cooling down %.0fs before re-checking %r",
        _zero_streak, _ZERO_BURST_COOLDOWN_SECS, query,
    )
    _defer_search(_ZERO_BURST_COOLDOWN_SECS)
    retry = search(query, timeout_secs, response_limit,
                   _throttle_probe=True, on_wait=on_wait)
    if retry:
        logger.warning("Throttling confirmed: %r found %d peer(s) after the "
                       "cooldown", query, len(retry))
    else:
        _zero_verified_until = time.monotonic() + _ZERO_VERIFIED_WINDOW_SECS
        logger.info("Re-check still empty — trusting empty results for the "
                    "next %.0fs", _ZERO_VERIFIED_WINDOW_SECS)
    return retry


def _search_once(query, timeout_secs, response_limit, on_wait=None):
    """One serialized, paced search. Holds the global search lock from the
    pacing wait through the response read, so searches never overlap and
    never start closer than ``SEARCH_MIN_INTERVAL_SECS`` apart.

    Lifecycle (empirically verified against the slskd/slskd:latest image):

    * Mid-search neither ``state(includeResponses=True)`` nor ``/responses``
      returns the accumulated peer responses — slskd holds them in memory
      until something forces a flush to the response collection.
    * Calling ``stop()`` (PUT) is the one path that triggers that
      flush: within ~200ms the search transitions to ``isComplete=True`` and
      ``/responses`` returns the full set. The ``Cancelled`` flag the slskd
      source sets in that path is cosmetic — responses are preserved.
    * We exit the polling loop as soon as the file count plateaus for 6s
      (peers stopped reporting) or slskd marks the search complete on its
      own, then call stop() to force the flush, then read ``/responses``.
    * **No global stale-search cleanup.** Each search only deletes itself.
    """
    with _search_lock:
        state = _start_search(query, timeout_secs, response_limit, on_wait=on_wait)

        search_id = state["id"]
        logger.info("Search started: id=%s query=%r", search_id, query)

        # Poll for stability or natural completion.
        start = time.monotonic()
        last_count = -1
        stable_since = None
        file_count = 0
        while time.monotonic() - start < timeout_secs:
            time.sleep(0.5)
            try:
                state = _request("GET", "/searches/%s" % search_id)
            except Exception:
                continue
            file_count = state.get("fileCount", 0)
            if state.get("isComplete"):
                break
            if file_count != last_count:
                last_count = file_count
                stable_since = time.monotonic()
            elif stable_since and (time.monotonic() - stable_since) > 6 and file_count > 0:
                break

        # Force a flush so /responses populates. stop() is the only API that
        # does this for our slskd version.
        try:
            _request("PUT", "/searches/%s" % search_id)
        except Exception:
            pass
        # Brief wait for slskd to mark complete and persist responses.
        for _ in range(8):
            time.sleep(0.25)
            try:
                state = _request("GET", "/searches/%s" % search_id)
            except Exception:
                break
            if state.get("isComplete"):
                break

        responses = []
        try:
            responses = _request("GET", "/searches/%s/responses" % search_id) or []
        except Exception:
            pass

        logger.info("Search done: %d files / %d peers", file_count, len(responses))

        try:
            _request("DELETE", "/searches/%s" % search_id)
        except Exception:
            pass

        return responses


# --- parse -------------------------------------------------------------------

def _flatten(responses, lossless_only=True):
    """Convert slskd peer responses into flat SearchResult list, audio-extensions only."""
    allowed = LOSSLESS_EXTS if lossless_only else AUDIO_EXTS
    out = []
    for resp in responses:
        username = resp.get("username", "")
        free = bool(resp.get("hasFreeUploadSlot", False))
        speed = int(resp.get("uploadSpeed", 0) or 0)
        queue = int(resp.get("queueLength", 0) or 0)
        for f in resp.get("files", []):
            fname = f.get("filename", "") or ""
            if not fname:
                continue
            ext = fname.rsplit(".", 1)[-1].lower() if "." in fname else ""
            if ext not in allowed:
                continue
            size = int(f.get("size", 0) or 0)
            out.append(SearchResult(
                username=username,
                filename=fname,
                size=size,
                bit_rate=f.get("bitRate"),
                bit_depth=f.get("bitDepth"),
                sample_rate=f.get("sampleRate"),
                length=f.get("length"),
                has_free_slot=free,
                upload_speed=speed,
                queue_length=queue,
            ))
    return out


def parse_files(responses, lossless_only=True):
    """Public: flatten search responses to SearchResult list with audio-ext filter."""
    return _flatten(responses, lossless_only=lossless_only)


def group_by_folder(results):
    """Group results into per-(user, directory) folders for album-level matching."""
    bucket = {}
    for r in results:
        key = (r.username, r.directory)
        if key not in bucket:
            bucket[key] = PeerFolder(
                username=r.username,
                directory=r.directory,
                has_free_slot=r.has_free_slot,
                upload_speed=r.upload_speed,
                queue_length=r.queue_length,
            )
        bucket[key].files.append(r)
    return list(bucket.values())


# --- enqueue + monitor ------------------------------------------------------

def enqueue(username, files):
    """Queue files for download from a single peer. Returns True on success."""
    payload = [{"filename": f.filename, "size": f.size} for f in files]
    try:
        _request("POST", "/transfers/downloads/%s" % urllib.parse.quote(username, safe=""),
                 payload)
        logger.debug("Enqueued %d file(s) from %s", len(files), username)
        return True
    except Exception as exc:
        logger.warning("Failed to enqueue from %s: %s", username, exc)
        return False


def _flatten_downloads(raw):
    """slskd returns nested [{username, directories: [{directory, files: [...]}]}].
    Flatten to a list of file dicts each with username injected."""
    out = []
    for entry in raw or []:
        u = entry.get("username", "")
        for d in entry.get("directories", []) or []:
            for fe in d.get("files", []) or []:
                fe = dict(fe)
                fe["username"] = u
                out.append(fe)
    return out


def get_downloads(username=None):
    try:
        if username:
            raw = _request("GET", "/transfers/downloads/%s" % urllib.parse.quote(username, safe=""))
            raw = [raw] if raw else []
        else:
            raw = _request("GET", "/transfers/downloads/")
    except Exception as exc:
        logger.debug("get_downloads failed: %s", exc)
        return []
    return _flatten_downloads(raw)


def _state_is_complete(state):
    s = (state or "").lower()
    return "completed" in s and "succeeded" in s


def _state_is_failed(state):
    s = (state or "").lower()
    if not s:
        return False
    if "completed" in s and "succeeded" in s:
        return False
    return any(k in s for k in ("errored", "rejected", "timedout", "cancelled", "failed"))


def _state_is_terminal(state):
    return _state_is_complete(state) or _state_is_failed(state)


def get_active_download_state(username, filename):
    """State of a live (queued or in-progress) download of this exact remote
    file, or None. Lets callers attach to transfers that survived a bot
    restart instead of re-enqueueing them — slskd rejects duplicate enqueues
    of an active transfer. Terminal history rows are ignored."""
    for row in get_downloads(username):
        state = row.get("state", "") or ""
        if row.get("filename") == filename and not _state_is_terminal(state):
            return state or "Queued"
    return None


_QUEUE_POSITION_INTERVAL_SECS = 30.0
_QUEUE_HEARTBEAT_SECS = 5.0
# A transfer that has left the queue but moved no bytes for this long is
# given up on, so the caller moves to the next peer instead of holding the
# slot until the per-attempt timeout (15 min by default). 0 disables it.
# Waiting in a peer's queue never counts — that stays with the timeout.
TRANSFER_STALL_SECS = int(os.environ.get("SLSKD_STALL_SECS", "120"))
STALLED_STATE = "Stalled, ClientSide"


def _get_queue_position(username, transfer_id):
    """Best-effort remote queue position; never part of transfer correctness."""
    try:
        value = _request("GET", "/transfers/downloads/%s/%s/position"
                         % (urllib.parse.quote(username, safe=""),
                            urllib.parse.quote(transfer_id, safe="")))
        position = int(value)
        return position if position > 0 else None
    except Exception:
        return None


def wait_for_files(username, target_filenames, timeout_secs=600,
                   poll_interval=2.0, progress_cb=None, state_cb=None,
                   stall_secs=None):
    """Poll slskd until each target filename reports completed or failed.

    ``progress_cb`` is called as ``cb(filename, pct, state, speed_bps,
    bytes_transferred, size)`` whenever any of the visible fields change.
    ``state_cb`` is called as ``cb(filename, state, queue_position,
    queued_seconds)`` on state changes and while remotely queued.

    Returns ``{filename: state}`` for every requested file. ``state`` is
    "Completed, Succeeded" on success or whatever slskd reports otherwise —
    or ``STALLED_STATE`` when a started transfer moved no bytes for
    ``stall_secs`` (default ``TRANSFER_STALL_SECS``; 0 disables the check).
    """
    if stall_secs is None:
        stall_secs = TRANSFER_STALL_SECS
    pending = set(target_filenames)
    states = {}
    start = time.monotonic()
    last_progress = {}
    last_state = {}
    last_state_notice = {}
    queue_since = {}
    queue_positions = {}
    last_lookup = {}
    best_bytes = {}
    last_gain = {}

    while pending and (time.monotonic() - start) < timeout_secs:
        rows = get_downloads(username)
        for row in rows:
            fname = row.get("filename", "")
            if fname not in pending:
                continue
            now = time.monotonic()
            state = row.get("state", "") or ""
            pct = float(row.get("percentComplete", 0) or 0)
            speed = int(row.get("averageSpeed", 0) or 0)
            bytes_done = int(row.get("bytesTransferred", 0) or 0)
            size = int(row.get("size", 0) or 0)
            visible = (pct, state, speed, bytes_done, size)
            if progress_cb and visible != last_progress.get(fname):
                last_progress[fname] = visible
                try:
                    progress_cb(fname, pct, state, speed, bytes_done, size)
                except Exception:
                    pass

            old_state = last_state.get(fname)
            state_changed = state != old_state
            if state_changed:
                logger.info("Transfer state changed for %s from %s: %s -> %s",
                            fname, username, old_state or "unseen", state or "unknown")
                last_state[fname] = state

            remotely_queued = "queued" in state.lower() and "remotely" in state.lower()
            position_changed = False
            if remotely_queued:
                queue_since.setdefault(fname, now)
                transfer_id = row.get("id")
                if (transfer_id
                        and now - last_lookup.get(fname, float("-inf")) >=
                        _QUEUE_POSITION_INTERVAL_SECS):
                    last_lookup[fname] = now
                    position = _get_queue_position(username, transfer_id)
                    if position != queue_positions.get(fname):
                        queue_positions[fname] = position
                        position_changed = True
                        if position is not None:
                            logger.info("Remote queue position for %s from %s: %d",
                                        fname, username, position)
                heartbeat = now - last_state_notice.get(fname, float("-inf")) >= \
                    _QUEUE_HEARTBEAT_SECS
                if state_cb and (state_changed or position_changed or heartbeat):
                    last_state_notice[fname] = now
                    try:
                        state_cb(fname, state, queue_positions.get(fname),
                                 now - queue_since[fname])
                    except Exception:
                        pass
            else:
                queue_since.pop(fname, None)
                queue_positions.pop(fname, None)
                if state_cb and state_changed:
                    last_state_notice[fname] = now
                    try:
                        state_cb(fname, state, None, 0.0)
                    except Exception:
                        pass

            if _state_is_complete(state) or _state_is_failed(state):
                states[fname] = state
                pending.discard(fname)
                continue

            # Stall guard: only once the peer has started sending. The
            # clock restarts whenever the byte count grows, so a slow but
            # moving transfer is never cut off.
            if not stall_secs or "queued" in state.lower():
                last_gain.pop(fname, None)
            elif fname not in last_gain or bytes_done > best_bytes[fname]:
                best_bytes[fname] = bytes_done
                last_gain[fname] = now
            elif now - last_gain[fname] >= stall_secs:
                logger.info("Transfer of %s from %s stalled: no bytes for %ds "
                            "at %.0f%% — giving up on this peer",
                            fname, username, stall_secs, pct)
                states[fname] = STALLED_STATE
                pending.discard(fname)
        if pending:
            time.sleep(poll_interval)

    # Anything still pending at timeout is reported as TimedOut.
    for fname in pending:
        state = last_state.get(fname, "")
        if "queued" in state.lower() and "remotely" in state.lower():
            states[fname] = "TimedOut, ClientSide, QueuedRemotely"
        else:
            states[fname] = "TimedOut, ClientSide, Transfer"
    return states


def cancel_download(username, filename, remove=True):
    """Cancel a queued/in-progress download and optionally remove it from history.

    slskd's cancel endpoint keys on the transfer GUID, not the filename (the id
    is interpolated straight into the path). Passing the filename always 404s,
    which used to be swallowed silently — so stalled transfers were never
    actually cancelled and kept eating bandwidth + staging disk. Resolve the
    live transfer's id first, and surface a real failure.
    """
    transfer_id = None
    for row in get_downloads(username):
        if row.get("filename") == filename:
            transfer_id = row.get("id")
            break
    if not transfer_id:
        logger.debug("cancel_download: no live transfer for %s / %s", username, filename)
        return
    try:
        _request("DELETE", "/transfers/downloads/%s/%s?remove=%s"
                 % (urllib.parse.quote(username, safe=""),
                    urllib.parse.quote(str(transfer_id), safe=""),
                    "true" if remove else "false"))
    except Exception as exc:
        logger.warning("cancel_download failed for %s (%s): %s", filename, transfer_id, exc)


def remove_completed_downloads():
    """Prune terminal rows from slskd's download history. Left unchecked they
    accumulate forever and can shadow a re-enqueue of the same filename in
    wait_for_files (a stale 'Completed, Errored' row resolves the new wait
    instantly and wrongly). Call after each album."""
    try:
        _request("DELETE", "/transfers/downloads/all/completed")
    except Exception as exc:
        logger.debug("remove_completed_downloads failed: %s", exc)


# --- file location -----------------------------------------------------------

def _safe_username(username):
    """Constrain a peer-supplied username to chars safe for path joins.
    Stripping path separators alone isn't enough — a literal ``..`` survives
    the char filter and still resolves to the parent dir on join."""
    cleaned = _SAFE_USERNAME_RE.sub("_", username)[:128]
    cleaned = cleaned.lstrip(".")
    # A ".." anywhere inside the component is harmless to os.path.join
    # (it is a literal name, not traversal), but strip it anyway so a
    # cleaned name can never be confused with a parent-dir reference.
    while ".." in cleaned:
        cleaned = cleaned.replace("..", "_")
    return cleaned or "_unknown"


def find_local_file(download_dir, username, remote_filename):
    """Locate the on-disk file slskd wrote for a completed transfer.

    slskd typically writes to ``<download_dir>/<username>/<remote_dir>/<basename>``,
    but it occasionally rewrites the directory part. We do a focused walk under
    the user dir first, then a broader scan as a last resort.
    """
    basename = remote_filename.rsplit("\\", 1)[-1] if "\\" in remote_filename \
        else remote_filename.rsplit("/", 1)[-1]

    user_dir = os.path.join(download_dir, _safe_username(username))
    if os.path.isdir(user_dir):
        for root, _, files in os.walk(user_dir):
            if basename in files:
                return os.path.join(root, basename)

    if os.path.isdir(download_dir):
        for root, _, files in os.walk(download_dir):
            if basename in files:
                return os.path.join(root, basename)

    return None
