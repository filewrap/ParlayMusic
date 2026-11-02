"""Download orchestrator: Soulseek search results -> files on disk.

Ported from music-bot-navidrome `src/soulseek/downloader.py`, adapted
to Parlay: the ref downloads into a Navidrome library (tag-based
dedup, cover art, lyrics, forced tag rewrite); Parlay has no library
and does no tagging, so this module downloads into the service
download dir with sanitized names and returns plain result dicts.

Flow per album:
  1. try album-folder match on slskd; if no luck, per-track matching
  2. enqueue chosen files to slskd, wait for completion
  3. move from slskd's download dir into the destination dir

Flow per single track: per-track matching only (no folder phase).

Env (see client.py): SLSKD_URL, SLSKD_API_KEY, SLSKD_DOWNLOAD_DIR,
SLSKD_DOWNLOAD_TIMEOUT (default 900), SLSKD_MAX_PEER_ATTEMPTS
(default 3), SLSKD_SLOW_MIN_MBPS (default 0 = disabled).
"""

import logging
import os
import time

from . import client as slskd
from .client import SearchResult
from .matcher import find_album, find_track
from .scorer import ScoredFolder
from .selection import (
    PEER_ABANDON_K,
    SlowSourceMonitor,
    demote_measured_slow,
    find_slow_source_switch,
    order_for_download,
    plan_folder_phase,
    quality_label,
    slow_floor_mbps,
)
from ..util import sanitize_filename

logger = logging.getLogger(__name__)

# Where slskd dumps completed downloads (mounted into both slskd and
# this service's containers). Override via SLSKD_DOWNLOAD_DIR.
# Total queue-plus-download cap for one peer attempt (seconds). It is one
# shared 15-minute window, not separate queue and transfer timeouts.
DOWNLOAD_TIMEOUT_SECS = int(os.environ.get("SLSKD_DOWNLOAD_TIMEOUT", "900"))

# How many peers a single-track download tries before giving up. Album tracks
# already try every candidate; this is the cap for one pasted track, where a
# popular release often has more good copies than the first three.
MAX_PEER_ATTEMPTS = max(1, int(os.environ.get("SLSKD_MAX_PEER_ATTEMPTS", "3")))


class PeerTransferError(Exception):
    """Signals a per-peer issue we should retry the next candidate for —
    enqueue rejected, transfer stalled, file never landed in slskd's dir.
    Local I/O errors are *not* this; they propagate as their own
    exception types so a real bug doesn't get silently masked as 'try next
    peer'."""


def _no_match_reason(diag):
    """Honest user-facing reason for an empty candidate list, from the
    find_track diagnostics: distinguish 'Soulseek has nothing at all' from
    'peers only have lossy rips' from 'files exist but none credibly match'."""
    if diag.get("usable_seen"):
        return "files found but none matched this track"
    if diag.get("lossy_dropped"):
        label = diag.get("best_lossy_label") or "lossy"
        return "only lossy copies found (%s)" % label
    if diag.get("files_seen"):
        return "files found but none matched this track"
    return "nothing found on Soulseek"


def _format_label_from_result(r):
    ext = r.extension.upper()
    bd = r.bit_depth
    sr = r.sample_rate
    parts = [ext]
    if bd:
        parts.append("%s-bit" % bd)
    if sr:
        parts.append("%skHz" % (sr // 1000))
    return " ".join(parts) if parts else ext


def _await_one_file(username, remote_filename, timeout_secs=DOWNLOAD_TIMEOUT_SECS):
    """Wait for slskd to finish a single file. Returns the final state string."""
    states = slskd.wait_for_files(
        username=username,
        target_filenames=[remote_filename],
        timeout_secs=timeout_secs,
    )
    return states.get(remote_filename, "Unknown")


def _ensure_enqueued(chosen):
    """Idempotent enqueue. After a bot restart slskd is often still
    downloading the very file a resumed request picks again — and slskd
    rejects duplicate enqueues of an active transfer. Attach to a live
    transfer when one exists; otherwise enqueue; and when the enqueue is
    refused, re-check for a live row before declaring the peer broken (the
    refusal may *be* the duplicate rejection racing our first check)."""
    active = slskd.get_active_download_state(chosen.username, chosen.filename)
    if active is not None:
        logger.info("Attaching to existing slskd transfer for %s (state: %s)",
                    chosen.basename, active)
        return
    if slskd.enqueue(chosen.username, [chosen]):
        return
    active = slskd.get_active_download_state(chosen.username, chosen.filename)
    if active is None:
        raise PeerTransferError("slskd refused enqueue from %s" % chosen.username)
    logger.info("Enqueue refused but transfer is live (state: %s) — attaching "
                "to it for %s", active, chosen.basename)


def _download_chosen(chosen, dest_dir, name):
    """Enqueue a single file, wait, move to dest_dir.

    Returns ``(filepath, bytes, fmt_label)``. Raises
    ``PeerTransferError`` for per-peer issues (caller retries another peer);
    other exceptions (OSError on move, ...) propagate as-is.
    """
    _ensure_enqueued(chosen)
    state = _await_one_file(chosen.username, chosen.filename)
    if "succeeded" not in state.lower():
        try:
            slskd.cancel_download(chosen.username, chosen.filename)
        except Exception:
            pass
        raise PeerTransferError("transfer ended in state '%s'" % state)

    src_path = slskd.find_local_file(
        slskd.slskd_download_dir(), chosen.username, chosen.filename)
    if not src_path or not os.path.isfile(src_path):
        raise PeerTransferError(
            "could not locate downloaded file for %s" % chosen.basename)

    ext = chosen.extension if chosen.extension in ("flac", "m4a", "mp3") else "flac"
    dest_path = os.path.join(dest_dir, sanitize_filename(name) + "." + ext)
    try:
        os.rename(src_path, dest_path)
    except OSError:
        # Cross-device: copy + unlink.
        with open(src_path, "rb") as fsrc, open(dest_path, "wb") as fdst:
            while True:
                chunk = fsrc.read(1 << 20)
                if not chunk:
                    break
                fdst.write(chunk)
        os.unlink(src_path)
    size = os.path.getsize(dest_path)
    fmt = _format_label_from_result(chosen)
    return dest_path, size, fmt


def _try_candidates(candidates, dest_dir, name, max_attempts=None,
                    failed_keys=None):
    """Try peers in order; return on first success. Only ``PeerTransferError``
    is treated as 'try the next peer' — any other exception (local I/O,
    etc.) is a real bug and propagates so it doesn't get silently
    swallowed as a peer issue.

    ``failed_keys`` (optional) is a (username, filename) set shared across an
    album's phases: candidates that already failed there are skipped, and new
    failures are recorded — so the per-track fallback retries *other* peers
    instead of re-waiting on the one that just errored.

    ``max_attempts`` defaults to ``MAX_PEER_ATTEMPTS``."""
    if max_attempts is None:
        max_attempts = MAX_PEER_ATTEMPTS
    seen = set(failed_keys or ())
    last_err = None
    attempts = 0
    for cand in candidates:
        if cand is None:
            continue
        key = (cand.username, cand.filename)
        if key in seen:
            continue
        seen.add(key)
        if attempts >= max_attempts:
            break
        attempts += 1
        try:
            filepath, size, fmt = _download_chosen(cand, dest_dir, name)
            return filepath, size, fmt, cand
        except PeerTransferError as e:
            last_err = str(e)
            if failed_keys is not None:
                failed_keys.add(key)
            logger.warning("candidate %s failed for %s: %s",
                           cand.username, name, e)
            continue
    if last_err:
        raise PeerTransferError(last_err)
    if attempts == 0 and failed_keys and any(c is not None for c in candidates):
        raise PeerTransferError("all matching peers already failed this album")
    return None


def find_lossy_candidates(track, album_artist=None):
    """Return mp3/m4a peer candidates for a track. Used by the mp3-fallback
    path to preview availability before asking the user to accept."""
    auto, alternatives = find_track(
        track, album_artist=album_artist, accept_lossy=True,
    )
    return order_for_download(auto, alternatives)


def download_single_track(track, dest_dir, accept_lossy=False,
                          precomputed_candidates=None):
    """Download one track. ``track`` is a Parlay track dict
    ({title, artist, duration_sec?}). Returns
    ``(filepath, was_downloaded, format_label, peer_username)``.
    Raises RuntimeError with an honest reason when Soulseek has no
    usable copy (or slskd is unreachable — SearchError propagates).
    """
    os.makedirs(dest_dir, exist_ok=True)
    title = track.get("title", "")
    artist = track.get("artist", "")
    name = "%s - %s" % (artist or "soulseek", title or "track")

    best_score = None
    if precomputed_candidates is not None:
        candidates = list(precomputed_candidates)
    else:
        diag = {}
        auto, alternatives = find_track(
            track, accept_lossy=accept_lossy, diag=diag,
        )
        top = auto or (alternatives[0] if alternatives else None)
        if top is not None:
            best_score = top.match_score
        candidates = order_for_download(auto, alternatives)

        if not candidates:
            reason = _no_match_reason(diag)
            if accept_lossy:
                raise RuntimeError(
                    "No mp3/m4a fallback either for %s — %s (%s)"
                    % (artist, title, reason)
                )
            raise RuntimeError(
                "No FLAC found on Soulseek for %s — %s (%s)"
                % (artist, title, reason)
            )

    if not candidates:
        raise RuntimeError("No candidates for %s — %s" % (artist, title))

    logger.info("Soulseek match for %s — %s: peer=%s score=%s copies=%d",
                artist, title, candidates[0].username, best_score,
                len(candidates))
    t0 = time.monotonic()
    res = _try_candidates(candidates, dest_dir, name)
    if not res:
        raise RuntimeError("All candidate peers failed.")
    filepath, size, fmt, chosen = res
    elapsed = time.monotonic() - t0
    speed = (size / (1024 * 1024)) / elapsed if elapsed > 0 else 0
    logger.info(
        "Track: %s — %s | %.1fMB in %.1fs (%.1f MB/s) [%s] from %s",
        artist, title, size / (1024 * 1024),
        elapsed, speed, fmt, chosen.username,
    )
    return filepath, True, fmt, chosen.username


def download_album(album, dest_dir):
    """Download an album. ``album`` is a Parlay dict
    ({artist, title, tracks: [{title, artist?, duration_sec?}]}).
    Returns ``{album_dir, downloaded, skipped, failed, total, format,
    elapsed_secs, source_counts}``. No library dedup/tagging: Parlay
    has no library, so every track is downloaded fresh (``skipped``
    is always 0) and files land flat in ``dest_dir``.
    """
    slskd.remove_completed_downloads()

    artist = album.get("artist", "")
    title = album.get("title", "")
    tracks = album.get("tracks") or []
    total = len(tracks)
    album_t0 = time.monotonic()
    album_bytes = 0
    downloaded = 0
    failed = []
    format_label = ""
    logger.info("Album: %s — %s (%d tracks)", artist, title, total)

    to_download = [(i, t) for i, t in enumerate(tracks, 1)]
    remaining_track_ids = {str(i) for i, _ in to_download}
    last_error = {}
    # (username, filename) pairs that already failed a transfer this album —
    # shared across phases so retries go to *other* peers.
    failed_candidate_keys = set()
    slow_monitor = SlowSourceMonitor(slow_floor_mbps())
    abandoned_peers = set()
    track_provenance = {}

    def _attempt(i, track, candidates):
        """Try ``candidates`` for ``track`` in order. Updates album-level
        bookkeeping on success. Returns the SearchResult that succeeded,
        or ``None`` on failure (with ``last_error`` updated)."""
        nonlocal album_bytes, downloaded, format_label
        tid = str(i)
        name = "%s - %s" % (track.get("artist") or artist,
                            track.get("title") or "track")
        try:
            t0 = time.monotonic()
            res = _try_candidates(
                candidates, dest_dir, name,
                max_attempts=max(len(candidates), 1),
                failed_keys=failed_candidate_keys,
            )
            if res is None:
                raise PeerTransferError("no peer returned a usable file")
            filepath, size, fmt, chosen = res
            elapsed = time.monotonic() - t0
            mb = size / (1024 * 1024)
            speed = mb / elapsed if elapsed > 0 else 0
            if not format_label:
                format_label = fmt
            logger.info(
                "  [%d/%d] %s — %.1fMB in %.1fs (%.1f MB/s) [%s] from %s",
                i, total, track.get("title"), mb, elapsed, speed, fmt,
                chosen.username,
            )
            album_bytes += size
            downloaded += 1
            slow_monitor.record(chosen.username, size, elapsed)
            track_provenance[tid] = chosen.username
            remaining_track_ids.discard(tid)
            last_error.pop(tid, None)
            return chosen
        except PeerTransferError as e:
            reason = str(e)
            if reason in ("all matching peers already failed this album",
                          "no peer returned a usable file"):
                reason = last_error.get(tid, reason)
            last_error[tid] = reason
            logger.warning("  [%d/%d] track %s — failed: %s",
                           i, total, track.get("title"), e)
            return None

    # Folder-match wins outright when one peer has every track of the album:
    # single queue slot, no cross-version mixes, fewer flaky connections.
    folder_match, folder_alternatives, album_pool = find_album(album)

    n_tracks = len(tracks)
    plan = plan_folder_phase(folder_match, folder_alternatives, n_tracks)
    folder_chain = plan.chain if plan else []
    quality_lock = plan.quality_lock if plan else None
    if plan:
        label = "Album-folder match" if folder_match.missing_count == 0 \
            else "Partial-folder match (gap-fill %d/%d)" % (
                folder_match.missing_count, n_tracks)
        alt_note = " (+%d fallback peers)" % len(folder_alternatives) \
            if folder_alternatives else ""
        logger.info("%s: peer=%s score=%.1f covers %d/%d tracks @ %s%s",
                    label, folder_match.folder.username, folder_match.score,
                    n_tracks - folder_match.missing_count, n_tracks,
                    quality_label(quality_lock), alt_note)

    def _consider_slow_switch(current_rank):
        """Between-tracks slow-source verdict. Returns the chain rank to
        switch to, or None to keep walking."""
        peer = folder_chain[current_rank].folder.username
        if not remaining_track_ids or not slow_monitor.wants_switch(peer):
            return None
        avg = slow_monitor.avg_mbps(peer)
        n_samples = slow_monitor.sample_count(peer)
        target = find_slow_source_switch(
            slow_monitor, peer, folder_chain, tracks,
            remaining_track_ids, quality_lock, abandoned_peers)
        if target is None:
            slow_monitor.settle()
            logger.info(
                "Slow source: peer %s avg %.2f MB/s over %d track(s) (floor %g)"
                " — staying: no chain folder covers %d remaining @ %s",
                peer, avg, n_samples, slow_monitor.floor_mbps,
                len(remaining_track_ids), quality_label(quality_lock),
            )
            return None
        rank, chosen = target
        slow_monitor.note_switch()
        logger.info(
            "Slow source: peer %s avg %.2f MB/s over %d track(s) (floor %g)"
            " — switching to peer %s (chain rank %d, switch %d/%d)",
            peer, avg, n_samples, slow_monitor.floor_mbps,
            chosen.folder.username, rank,
            slow_monitor.switches_used, 2,
        )
        return rank

    # === Folder-chain phase ===
    upcoming = list(range(len(folder_chain)))
    walked = 0
    while upcoming and remaining_track_ids:
        folder_rank = upcoming.pop(0)
        folder = folder_chain[folder_rank]
        first_walked = walked == 0
        walked += 1
        folder_chosen = {}
        for pos, (tr, sr) in enumerate(zip(tracks, folder.matched_files)):
            idx = pos + 1
            if str(idx) in remaining_track_ids and sr is not None:
                folder_chosen[str(idx)] = sr
        if not folder_chosen:
            continue
        if not first_walked:
            logger.info(
                "Folder fallback #%d: peer=%s score=%.1f covers %d remaining track(s)",
                folder_rank, folder.folder.username, folder.score, len(folder_chosen),
            )
        failures_in_a_row = 0
        switch_to = None
        for i, track in to_download:
            if str(i) not in folder_chosen:
                continue
            if str(i) not in remaining_track_ids:
                continue
            used = _attempt(i, track, [folder_chosen[str(i)]])
            if used is not None:
                failures_in_a_row = 0
                switch_to = _consider_slow_switch(folder_rank)
                if switch_to is not None:
                    break
            else:
                failures_in_a_row += 1
                if failures_in_a_row >= PEER_ABANDON_K:
                    logger.warning(
                        "Abandoning peer %s after %d consecutive failures, switching to next folder",
                        folder.folder.username, failures_in_a_row,
                    )
                    abandoned_peers.add(folder.folder.username)
                    break
        if switch_to is not None:
            upcoming = [switch_to] + [r for r in upcoming if r != switch_to]

    # === Per-track fallback phase ===
    if remaining_track_ids:
        needs_per_track = [(i, t) for (i, t) in to_download
                           if str(i) in remaining_track_ids]
        logger.info("Per-track matching for %d remaining track(s)%s",
                    len(needs_per_track),
                    ", reusing %d pooled result(s)" % len(album_pool) if album_pool else "")
        search_dead = None  # searching stopped being useful — why
        for i, track in needs_per_track:
            diag = {}
            try:
                # Once searching has proven throttled/broken, keep matching
                # the remaining tracks against the pool only — a pool hit
                # still downloads fine without touching the network.
                auto, alts = find_track(
                    track, album_artist=artist, preseed=album_pool,
                    allow_search=search_dead is None, diag=diag,
                )
            except slskd.SearchError as e:
                # Not a miss — stop searching so we don't dig the throttle
                # hole deeper; unmatched tracks report the real reason.
                search_dead = "search rate-limited — retry later" \
                    if isinstance(e, slskd.SearchThrottledError) \
                    else "Soulseek search failed — retry later"
                logger.warning("Search failed on %s (%s) — pool-only matching "
                               "from here on", track.get("title"), e)
                diag = {}
                auto, alts = find_track(
                    track, album_artist=artist, preseed=album_pool,
                    allow_search=False, diag=diag,
                )
            # Measured-slow peers go to the back — still eligible (better a
            # slow track than a missing one), but gap-fill shouldn't
            # immediately reward the peer the folder walk just left.
            candidates = demote_measured_slow(
                order_for_download(auto, alts, quality_lock), slow_monitor)
            if not candidates:
                reason = search_dead or _no_match_reason(diag)
                last_error.setdefault(str(i), reason)
                continue
            used = _attempt(i, track, candidates)
            if used is not None and quality_lock is None:
                quality_lock = (used.bit_depth, used.sample_rate)

    # Tally final failures from whatever's still unfilled.
    for i, track in to_download:
        if str(i) in remaining_track_ids:
            failed.append((track.get("title"),
                           last_error.get(str(i), "nothing found on Soulseek")))

    elapsed = time.monotonic() - album_t0
    mb = album_bytes / (1024 * 1024)
    avg_speed = mb / elapsed if elapsed > 0 else 0
    logger.info(
        "Album done: %s — %s | %.0fMB, %d downloaded, %d failed in %.0fs (%.1f MB/s)",
        artist, title, mb, downloaded, len(failed),
        elapsed, avg_speed,
    )

    source_counts = {}
    for peer in track_provenance.values():
        source_counts[peer] = source_counts.get(peer, 0) + 1

    return {
        "album_dir": dest_dir,
        "downloaded": downloaded,
        "skipped": 0,
        "failed": failed,
        "total": total,
        "format": format_label,
        "elapsed_secs": elapsed,
        "source_counts": source_counts,
    }
