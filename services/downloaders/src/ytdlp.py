"""yt-dlp subprocess wrapper.

The downloader service is stdlib-only apart from the yt-dlp *binary*
invoked as a subprocess (installed in .venv, see requirements.txt).
No yt-dlp Python API import: pinning to the CLI keeps the server working
even if the library layout changes, and keeps the HTTP layer stdlib-only.

Behaviour ported from sanila's bot.py: search for the query, take the
best match, download bestaudio and extract mp3.
"""
import json
import os
import re
import shutil
import subprocess
import tempfile
import threading
import uuid

from .util import (ApiError, download_failed, engine_unavailable,
                   parse_duration, sanitize_filename, track_dict)

YTDLP = shutil.which("yt-dlp") or os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    ".venv", "bin", "yt-dlp")

SEARCH_TIMEOUT_S = 150
DOWNLOAD_TIMEOUT_S = 900


def _check_ytdlp():
    if not YTDLP or not os.path.isfile(YTDLP):
        raise engine_unavailable(
            "yt-dlp binary not found; install requirements.txt into .venv")
    return YTDLP


def _run(args, timeout, extractor_args=None):
    ytdlp = _check_ytdlp()
    cmd = [ytdlp, "--no-warnings", "--quiet", "--no-check-certificate",
           "--socket-timeout", "30", "--retries", "3"]
    if extractor_args:
        cmd += ["--extractor-args", extractor_args]
    cmd += args
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=timeout)
    except subprocess.TimeoutExpired:
        raise download_failed("yt-dlp timed out after %ss" % timeout)
    except OSError as exc:
        raise engine_unavailable("cannot run yt-dlp: %s" % exc)
    return proc


def _is_bot_check(stderr):
    return "not a bot" in stderr or "Sign in to confirm" in stderr


_SIZE_UNITS = {"B": 1, "KB": 1000, "MB": 1000 ** 2, "GB": 1000 ** 3,
               "TB": 1000 ** 4, "KIB": 1024, "MIB": 1024 ** 2,
               "GIB": 1024 ** 3, "TIB": 1024 ** 4}

_PROGRESS_RE = re.compile(
    r"\[download\]\s+(\d+(?:\.\d+)?)% of\s+~?\s*([0-9.]+)\s*([KMGT]?i?B)"
    r"(?:\s+at\s+([0-9.]+)\s*([KMGT]?i?B)/s)?"
    r"(?:\s+ETA\s+(\d+:\d+(?::\d+)?))?")


def _parse_size(num, unit):
    try:
        return int(float(num) * _SIZE_UNITS[unit.upper()])
    except (KeyError, ValueError):
        return 0


def _parse_hms(text):
    if not text:
        return None
    parts = [int(p) for p in text.split(":")]
    total = 0
    for p in parts:
        total = total * 60 + p
    return total


def parse_ytdlp_progress_line(line):
    """Parse one yt-dlp `--progress --newline` line into IPC-4 numbers.

    Returns {"percentage", "done_bytes", "total_bytes",
    "speed_bytes_per_sec", "eta_sec"} or None for non-progress lines
    (Destination / ExtractAudio / format lines). The numbers come
    straight off yt-dlp's own progress output — never estimated.
    Pure: exercised by tests/test_progress.py without any subprocess.
    """
    m = _PROGRESS_RE.search(line or "")
    if not m:
        return None
    pct = float(m.group(1))
    total = _parse_size(m.group(2), m.group(3))
    speed = _parse_size(m.group(4), m.group(5)) if m.group(4) else 0
    return {
        "percentage": pct,
        "done_bytes": int(total * pct / 100.0) if total else 0,
        "total_bytes": total or None,
        "speed_bytes_per_sec": speed,
        "eta_sec": _parse_hms(m.group(6)),
    }


def _run_streaming(args, timeout, extractor_args=None, progress_cb=None):
    """Run yt-dlp with `--progress --newline`, feeding progress lines
    to progress_cb as they arrive. Returns (returncode, stderr_text).

    stderr goes to a temp file (not a pipe) so a chatty yt-dlp can
    never deadlock the stdout read loop. `--progress` forces progress
    output even with `--quiet` (that is its documented purpose).
    stdout is drained on a daemon thread so the overall ``timeout``
    still caps the run exactly like `_run` does — a slow-but-steady
    download is cut off at the deadline, not left running.
    """
    ytdlp = _check_ytdlp()
    cmd = [ytdlp, "--no-warnings", "--quiet", "--progress", "--newline",
           "--no-check-certificate", "--socket-timeout", "30",
           "--retries", "3"]
    if extractor_args:
        cmd += ["--extractor-args", extractor_args]
    cmd += args
    err_fh = tempfile.TemporaryFile(mode="w+", encoding="utf-8",
                                    errors="replace")
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=err_fh,
                            text=True, bufsize=1)

    def _reader():
        try:
            for line in proc.stdout:
                if progress_cb:
                    parsed = parse_ytdlp_progress_line(line)
                    if parsed:
                        try:
                            progress_cb(**parsed)
                        except Exception:
                            pass  # a progress sink must never kill a download
        except Exception:
            pass

    reader = threading.Thread(target=_reader, daemon=True)
    reader.start()
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        raise download_failed("yt-dlp timed out after %ss" % timeout)
    except OSError as exc:
        raise engine_unavailable("cannot run yt-dlp: %s" % exc)
    reader.join(timeout=5)
    err_fh.seek(0)
    stderr = err_fh.read()
    err_fh.close()
    return proc.returncode, stderr


def _entry_to_track(entry, source="youtube"):
    """Map one yt-dlp flat-playlist entry onto the contract Track type."""
    vid = entry.get("id") or entry.get("url") or ""
    title = entry.get("title") or vid
    artist = entry.get("uploader") or entry.get("channel") or entry.get("artist")
    url = entry.get("webpage_url") or entry.get("url")
    if url and not url.startswith("http"):
        url = "https://www.youtube.com/watch?v=" + str(url)
    thumbs = entry.get("thumbnails") or []
    thumbnail = thumbs[-1].get("url") if thumbs else None
    # IPC-3 (§29): views / uploader / upload_time / is_live from the
    # entry when yt-dlp reports them (flat search entries carry
    # view_count + upload_date; full info adds is_live/live_status).
    # `live` stays as the legacy field for back-compat.
    is_live = bool(entry.get("is_live")) or entry.get("live_status") in (
        "is_live", "is_upcoming")
    track = track_dict(vid, title, source, artist=artist,
                       duration_sec=parse_duration(entry.get("duration")),
                       url=url, thumbnail=thumbnail,
                       views=entry.get("view_count"),
                       uploader=entry.get("uploader"),
                       upload_time=entry.get("upload_date"),
                       is_live=is_live)
    if is_live:
        track["live"] = True
    return track


def search_youtube(query, limit=5):
    """Search YouTube via yt-dlp ytsearch (flat, no downloads).

    A bare URL is resolved directly instead of searched.
    """
    limit = max(1, min(int(limit), 20))
    if query.startswith(("http://", "https://", "www.")):
        target = query
        args = ["--flat-playlist", "-J", "--no-playlist", target]
    else:
        args = ["--flat-playlist", "-J", "ytsearch%d:%s" % (limit, query)]
    proc = _run(args, SEARCH_TIMEOUT_S)
    if proc.returncode != 0 and _is_bot_check(proc.stderr):
        proc = _run(args, SEARCH_TIMEOUT_S,
                    extractor_args="youtube:player_client=android")
    if proc.returncode != 0:
        raise download_failed(
            "youtube search failed: %s" % (proc.stderr.strip() or "yt-dlp error"))
    try:
        info = json.loads(proc.stdout)
    except json.JSONDecodeError:
        raise download_failed("youtube search returned unparsable output")
    entries = info.get("entries") or []
    if not entries and info.get("id"):
        entries = [info]
    tracks = [_entry_to_track(e, "youtube") for e in entries[:limit] if e]
    return tracks


def download_youtube_audio(query_or_url, dest_dir, want_mp3=True,
                           progress_cb=None):
    """Download best audio for a query/URL; returns (file_path, track).

    Mirrors sanila's flow: search first (unless a URL was given), then
    download bestaudio and extract mp3 with ffmpeg (yt-dlp handles the
    post-processing; ffmpeg is present in this environment).

    ``progress_cb`` (optional) receives yt-dlp's own progress numbers
    as they arrive — ``cb(percentage, done_bytes, total_bytes,
    speed_bytes_per_sec, eta_sec)`` — so the IPC-4 progress snapshot
    reports the real transfer, never an estimate.
    """
    os.makedirs(dest_dir, exist_ok=True)
    if query_or_url.startswith(("http://", "https://", "www.")):
        target = query_or_url
    else:
        results = search_youtube(query_or_url, limit=1)
        if not results:
            raise download_failed("no YouTube match for query: %r" % query_or_url)
        target = results[0]["url"]

    job_id = uuid.uuid4().hex[:12]
    job_dir = os.path.join(dest_dir, "job-" + job_id)
    os.makedirs(job_dir, exist_ok=True)
    outtmpl = os.path.join(job_dir, "%(title).150B [%(id)s].%(ext)s")
    args = ["--format", "bestaudio/best",
            "--output", outtmpl,
            "--no-playlist",
            "--restrict-filenames",
            "--no-embed-metadata"]
    if want_mp3:
        args += ["--extract-audio", "--audio-format", "mp3",
                 "--audio-quality", "0"]
    args.append(target)
    # Streaming run (not _run): yt-dlp's --progress lines feed the
    # IPC-4 snapshot while the file is still transferring.
    rc, stderr = _run_streaming(args, DOWNLOAD_TIMEOUT_S,
                                progress_cb=progress_cb)
    if rc != 0 and _is_bot_check(stderr):
        # YouTube's bot check blocks the default (web) player client from
        # this egress IP; the android player client is not gated the same
        # way. Retry once with it — no cookies or credentials involved.
        rc, stderr = _run_streaming(
            args, DOWNLOAD_TIMEOUT_S,
            extractor_args="youtube:player_client=android",
            progress_cb=progress_cb)
    files = sorted(
        (os.path.join(job_dir, f) for f in os.listdir(job_dir)
         if os.path.isfile(os.path.join(job_dir, f))),
        key=os.path.getmtime)
    if rc != 0 or not files:
        raise download_failed(
            "youtube download failed: %s" % (stderr.strip() or "no file produced"))
    file_path = files[-1]

    # Track metadata from the source (cheap flat extract, no re-download).
    track = None
    try:
        info_proc = _run(["--flat-playlist", "-J", "--no-playlist", target],
                         SEARCH_TIMEOUT_S)
        if info_proc.returncode != 0 and _is_bot_check(info_proc.stderr):
            info_proc = _run(
                ["--flat-playlist", "-J", "--no-playlist", target],
                SEARCH_TIMEOUT_S,
                extractor_args="youtube:player_client=android")
        if info_proc.returncode == 0:
            info = json.loads(info_proc.stdout)
            track = _entry_to_track(info, "youtube")
    except (ApiError, json.JSONDecodeError, ValueError):
        track = None
    if track is None:
        base = sanitize_filename(os.path.splitext(os.path.basename(file_path))[0])
        track = track_dict(base, base, "youtube", url=target)
    return file_path, track


def extract_info_flat(url):
    """Flat (no download) info dict for a URL — used for Spotify pages."""
    proc = _run(["--flat-playlist", "-J", "--no-playlist", url],
                SEARCH_TIMEOUT_S)
    if proc.returncode != 0:
        return None
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError:
        return None
