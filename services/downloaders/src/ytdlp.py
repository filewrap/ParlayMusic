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
import shutil
import subprocess
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
    track = track_dict(vid, title, source, artist=artist,
                       duration_sec=parse_duration(entry.get("duration")),
                       url=url, thumbnail=thumbnail)
    if entry.get("live_status") in ("is_live", "is_upcoming") \
            or entry.get("is_live"):
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


def download_youtube_audio(query_or_url, dest_dir, want_mp3=True):
    """Download best audio for a query/URL; returns (file_path, track).

    Mirrors sanila's flow: search first (unless a URL was given), then
    download bestaudio and extract mp3 with ffmpeg (yt-dlp handles the
    post-processing; ffmpeg is present in this environment).
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
    proc = _run(args, DOWNLOAD_TIMEOUT_S)
    if (proc.returncode != 0 and _is_bot_check(proc.stderr)):
        # YouTube's bot check blocks the default (web) player client from
        # this egress IP; the android player client is not gated the same
        # way. Retry once with it — no cookies or credentials involved.
        proc = _run(args, DOWNLOAD_TIMEOUT_S,
                    extractor_args="youtube:player_client=android")
    files = sorted(
        (os.path.join(job_dir, f) for f in os.listdir(job_dir)
         if os.path.isfile(os.path.join(job_dir, f))),
        key=os.path.getmtime)
    if proc.returncode != 0 or not files:
        raise download_failed(
            "youtube download failed: %s" % (proc.stderr.strip() or "no file produced"))
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
