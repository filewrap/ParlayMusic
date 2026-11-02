"""TikTok audio downloads.

From telegram-music-bot-rust `main.rs::download_media` (yt-dlp download
of a TikTok URL; TikTok URLs skip the `youtube:player_client=android`
extractor arg) and `get_search_results` (yt-dlp metadata).

Two-stage path, both real:
1. Metadata + best-audio URL from yt-dlp (`-J --no-playlist`, then the
   `audio` format — an mp3 served straight off TikTok's CDN). yt-dlp
   extraction works fine from this sandbox (proven 2026-11-02).
2. The bytes: yt-dlp's own downloader stalls on reads from this
   egress proxy ("The read operation timed out", retries exhausted —
   proven 2026-11-02 on two videos), while a plain HTTP GET of the
   same signed URL downloads the whole file in ~2 s (711,792 bytes,
   128 kbps mp3). So the file is fetched with urllib. If the direct
   fetch fails, the module falls back to a native yt-dlp download
   (which works on networks without this egress behaviour).

The returned track carries a `note` naming the path taken.
"""
import os
import re
import subprocess
import urllib.request
import uuid

from .util import (download_failed, parse_duration, sanitize_filename,
                   track_dict)
from .ytdlp import _check_ytdlp, _run

_TIKTOK_RE = re.compile(
    r"(?:https?://)?(?:www\.|m\.)?(?:tiktok\.com/@[^/]+/video/"
    r"|tiktok\.com/v/|vm\.tiktok\.com/|vt\.tiktok\.com/)(\w+)")

_DIRECT_NOTE = ("audio extracted from TikTok video; downloaded direct "
                "from the TikTok CDN (yt-dlp's own downloader stalls on "
                "reads from this egress proxy)")
_YTDLP_NOTE = "audio extracted from TikTok video via yt-dlp"


def parse_tiktok_url(url):
    """Return the TikTok video id (or short code) or None."""
    if not isinstance(url, str):
        return None
    m = _TIKTOK_RE.search(url.strip())
    if not m:
        return None
    return m.group(1)


def _is_short_link(url):
    return bool(re.match(r"(?i)^https?://(vm|vt)\.tiktok\.com/", url.strip()))


def extract_info(url):
    """yt-dlp -J metadata for a TikTok URL (follows short links)."""
    ytdlp = _check_ytdlp()
    cmd = [ytdlp, "--no-warnings", "--quiet", "--no-playlist",
           "--no-check-certificate", "--socket-timeout", "30",
           "-J", url]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=90)
    except subprocess.TimeoutExpired:
        raise download_failed("yt-dlp timed out extracting TikTok metadata")
    except OSError as exc:
        raise download_failed("cannot run yt-dlp: %s" % exc)
    if proc.returncode != 0:
        raise download_failed(
            "TikTok metadata extract failed: %s"
            % (proc.stderr.strip() or "yt-dlp error"))
    import json
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError:
        raise download_failed("TikTok metadata returned unparsable output")


def _best_audio_url(info):
    """Pick the audio-only format URL (the mp3 TikTok serves directly)."""
    formats = info.get("formats") or []
    audio = [f for f in formats
             if f.get("acodec") not in (None, "none")
             and f.get("vcodec") in (None, "none")
             and f.get("url")]
    if audio:
        # Prefer mp3, else the highest-bitrate audio format.
        audio.sort(key=lambda f: (
            0 if f.get("ext") == "mp3" else 1,
            -(f.get("abr") or 0)))
        return audio[0]["url"]
    # No audio-only format: fall back to the combined format's URL and
    # let ffmpeg strip the video (yt-dlp native path handles this).
    return None


def _download_direct(url, dest_path):
    """Plain urllib GET of the signed media URL (the path that works)."""
    req = urllib.request.Request(
        url, headers={"User-Agent": "Parlay/0.1",
                      "Referer": "https://www.tiktok.com/"})
    try:
        with urllib.request.urlopen(req, timeout=180) as resp:
            if resp.status != 200:
                raise download_failed(
                    "TikTok CDN download: HTTP %s" % resp.status)
            with open(dest_path, "wb") as fh:
                while True:
                    chunk = resp.read(65536)
                    if not chunk:
                        break
                    fh.write(chunk)
    except download_failed:
        raise
    except Exception as exc:
        raise download_failed("TikTok CDN download failed: %s" % exc)
    if os.path.getsize(dest_path) < 1024:
        raise download_failed("TikTok CDN download produced an empty file")
    return dest_path


def _download_via_ytdlp(url, dest_dir):
    """Fallback: native yt-dlp download (works off this egress)."""
    job_dir = os.path.join(dest_dir, "job-" + uuid.uuid4().hex[:12])
    os.makedirs(job_dir, exist_ok=True)
    outtmpl = os.path.join(job_dir, "%(title).150B [%(id)s].%(ext)s")
    proc = _run(["--format", "bestaudio/best", "--output", outtmpl,
                 "--no-playlist", "--restrict-filenames",
                 "--extract-audio", "--audio-format", "mp3",
                 "--audio-quality", "0", url],
                timeout=600)
    files = sorted(
        (os.path.join(job_dir, f) for f in os.listdir(job_dir)
         if os.path.isfile(os.path.join(job_dir, f))),
        key=os.path.getmtime)
    if proc.returncode != 0 or not files:
        raise download_failed(
            "TikTok yt-dlp download failed: %s"
            % (proc.stderr.strip() or "no file produced"))
    return files[-1]


def download_tiktok(body):
    """Provider entry: {url} -> {file_path, track}."""
    url = (body.get("url") or body.get("query") or "").strip()
    if not url:
        raise download_failed("url is required")
    if not parse_tiktok_url(url):
        raise download_failed("not a TikTok video URL: %r" % url)
    dest = body["_dl_dir"]
    os.makedirs(dest, exist_ok=True)
    info = extract_info(url)
    vid = info.get("id") or parse_tiktok_url(url)
    title = info.get("title") or ("TikTok video %s" % vid)
    artist = info.get("uploader") or info.get("channel") or ""
    track = track_dict(vid, title, "tiktok", artist=artist,
                       duration_sec=parse_duration(info.get("duration")),
                       url=info.get("webpage_url") or url,
                       thumbnail=(info.get("thumbnail") or ""))
    name = sanitize_filename("%s - %s" % (artist or "tiktok",
                                          title or vid))
    dest_path = os.path.join(dest, name[:150] + ".mp3")
    audio_url = _best_audio_url(info)
    if audio_url:
        try:
            _download_direct(audio_url, dest_path)
            track["note"] = _DIRECT_NOTE
            return {"file_path": dest_path, "track": track}
        except Exception as exc:
            # Direct CDN fetch failed: fall through to native yt-dlp.
            track["note"] = "direct CDN fetch failed (%s); trying yt-dlp" % exc
    path = _download_via_ytdlp(url, dest)
    track["note"] = _YTDLP_NOTE
    return {"file_path": path, "track": track}
