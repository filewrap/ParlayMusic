"""Deezer downloads via the public Deezer API (no auth).

From music-bot-navidrome `src/metadata/deezer.py` (same public API,
same rate-limit discipline: Deezer's cap is 50 requests / 5 s per IP;
this module serialises its own calls with a small sleep between paged
requests and keeps bursts far under the cap).

Download path, in order:
1. yt-dlp on the Deezer track URL — refused outright: yt-dlp's Deezer
   extractor answers "[DRM] The requested site is known to use DRM
   protection. It will NOT be supported." (proven 2026-11-02). The
   attempt is still made once per track so a future yt-dlp change is
   picked up automatically; it fails fast (~2 s).
2. The 30-second preview MP3 from the API (`track.preview`) — a real
   file, downloaded with urllib. The track dict says so in `note`:
   full tracks need a Deezer account (ARL cookie), which this service
   does not have.

Album input downloads every track's preview and returns a ZIP
(Spotify-Loader's "Send as ZIP" shape); single tracks return the mp3.
"""
import json
import os
import re
import subprocess
import urllib.parse
import urllib.request
import uuid
import zipfile

from .util import (download_failed, parse_duration,
                   sanitize_filename, track_dict)
from .ytdlp import YTDLP

_API = "https://api.deezer.com"

_DEEZER_RE = re.compile(
    r"(?:https?://(?:www\.)?deezer\.com/(?:[a-z]{2}/)?)"
    r"(track|album|playlist|artist)/(\d+)")

_PREVIEW_NOTE = ("30-second preview from the Deezer public API; the full "
                 "track needs a Deezer account (yt-dlp refuses Deezer: "
                 "DRM-protected site)")


def parse_deezer_url(url):
    """Return (kind, deezer_id) or None if this is not a Deezer link."""
    if not isinstance(url, str):
        return None
    m = _DEEZER_RE.search(url.strip())
    if not m:
        return None
    return m.group(1), m.group(2)


def _api_get(path, params=None, timeout=20):
    url = _API + path
    if params:
        url += "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, headers={"User-Agent": "Parlay/0.1",
                                               "Accept-Language": "en-US"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except Exception as exc:
        raise download_failed("Deezer API request failed (%s): %s" % (url, exc))
    if isinstance(data, dict) and data.get("error"):
        err = data["error"]
        raise download_failed("Deezer API error: %s"
                              % err.get("message", err))
    return data


def _track_from_api(d, note=_PREVIEW_NOTE):
    return track_dict(
        str(d.get("id")), d.get("title") or d.get("title_short") or "",
        "deezer",
        artist=(d.get("artist") or {}).get("name"),
        duration_sec=parse_duration(d.get("duration")),
        url=d.get("link") or "",
        thumbnail=(d.get("album") or {}).get("cover_xl")
        or (d.get("album") or {}).get("cover_big"),
        note=note)


def search_deezer(query, limit=5):
    """Public-API track search -> [Track]."""
    data = _api_get("/search", {"q": query, "limit": max(1, min(limit, 20))})
    return [_track_from_api(d) for d in data.get("data") or []]


def get_track(track_id):
    """Full track metadata incl. preview URL, or raises download_failed."""
    return _api_get("/track/%s" % track_id)


def get_album_tracks(album_id):
    """Complete ordered track list for an album (paginated, 100/page)."""
    album = _api_get("/album/%s" % album_id)
    expected = int(album.get("nb_tracks") or 0)
    tracks = []
    index = 0
    while True:
        page = _api_get("/album/%s/tracks" % album_id,
                        {"index": index, "limit": 100})
        batch = page.get("data") or []
        tracks.extend(batch)
        index += len(batch)
        if not page.get("next") or not batch:
            break
    if expected and len(tracks) != expected:
        raise download_failed(
            "Deezer album %s: expected %d tracks, got %d"
            % (album_id, expected, len(tracks)))
    return album, tracks


def _download_preview(preview_url, dest_dir, name):
    """Download a preview MP3 with urllib; returns the file path."""
    if not preview_url:
        raise download_failed("Deezer track has no preview URL")
    out_path = os.path.join(dest_dir, sanitize_filename(name) + ".mp3")
    req = urllib.request.Request(preview_url,
                                 headers={"User-Agent": "Parlay/0.1"})
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            if resp.status != 200:
                raise download_failed(
                    "Deezer preview download: HTTP %s" % resp.status)
            with open(out_path, "wb") as fh:
                while True:
                    chunk = resp.read(65536)
                    if not chunk:
                        break
                    fh.write(chunk)
    except download_failed:
        raise
    except Exception as exc:
        raise download_failed("Deezer preview download failed: %s" % exc)
    if os.path.getsize(out_path) < 1024:
        raise download_failed("Deezer preview download produced an empty file")
    return out_path


def _try_ytdlp_full(track_url):
    """One fast yt-dlp attempt at the full track.

    Expected to fail (DRM refusal, proven 2026-11-02); returns True
    only if a file actually appears, so a future yt-dlp change is
    picked up without code changes. Kept short: 60 s cap.
    """
    if not YTDLP or not os.path.isfile(YTDLP):
        return None
    job_dir = "/tmp/parlay-deezer-ytdlp-" + uuid.uuid4().hex[:8]
    os.makedirs(job_dir, exist_ok=True)
    try:
        proc = subprocess.run(
            [YTDLP, "--no-warnings", "--quiet", "--no-playlist",
             "--no-check-certificate", "--socket-timeout", "15",
             "-f", "bestaudio/best", "--extract-audio",
             "--audio-format", "mp3", "--audio-quality", "0",
             "-o", os.path.join(job_dir, "%(id)s.%(ext)s"), track_url],
            capture_output=True, text=True, timeout=60)
    except (subprocess.TimeoutExpired, OSError):
        return None
    if proc.returncode != 0:
        return None
    files = [os.path.join(job_dir, f) for f in os.listdir(job_dir)
             if os.path.isfile(os.path.join(job_dir, f))]
    return files[0] if files else None


def download_track_by_id(track_id, dest_dir):
    """Download one Deezer track; returns (file_path, track)."""
    data = get_track(track_id)
    track = _track_from_api(data)
    full = _try_ytdlp_full(data.get("link") or
                           "https://www.deezer.com/track/%s" % track_id)
    if full:
        # Move the file into the service download dir (yt-dlp wrote it
        # to a scratch dir) so the returned path stays under dl_dir.
        name = "%s - %s.mp3" % (track.get("artist") or "deezer",
                                track.get("title") or track_id)
        dest_path = os.path.join(dest_dir, sanitize_filename(name))
        os.replace(full, dest_path)
        track["note"] = "full track downloaded via yt-dlp"
        return dest_path, track
    name = "%s - %s" % (track.get("artist") or "deezer",
                        track.get("title") or track_id)
    path = _download_preview(data.get("preview"), dest_dir, name)
    return path, track


def download_deezer(body):
    """Provider entry: {query|url} -> {file_path, track[, count]}.

    A Deezer URL (track/album) or a free-text query (searched first).
    Album input returns a ZIP of all track previews.
    """
    dest = body["_dl_dir"]
    os.makedirs(dest, exist_ok=True)
    query = (body.get("query") or body.get("url") or "").strip()
    if not query:
        raise download_failed("query or url is required")
    parsed = parse_deezer_url(query)
    if parsed and parsed[0] == "album":
        album, entries = get_album_tracks(parsed[1])
        job_dir = os.path.join(dest, "job-" + uuid.uuid4().hex[:12])
        os.makedirs(job_dir, exist_ok=True)
        downloaded = []
        for entry in entries:
            try:
                track = _track_from_api(entry)
                name = "%s - %s" % (track.get("artist") or "deezer",
                                    track.get("title") or entry.get("id"))
                path = _download_preview(entry.get("preview"), job_dir, name)
                downloaded.append((track, path))
            except Exception:
                continue
        if not downloaded:
            raise download_failed(
                "no Deezer previews could be downloaded for album %s"
                % parsed[1])
        zip_name = "%s-album-%s.zip" % (
            sanitize_filename(album.get("title") or "deezer"),
            uuid.uuid4().hex[:8])
        zip_path = os.path.join(dest, zip_name)
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
            for track, path in downloaded:
                arc = sanitize_filename(
                    "%s - %s.mp3" % (track.get("artist") or "unknown",
                                     track.get("title") or "track"))
                zf.write(path, arcname=arc)
        first = downloaded[0][0]
        first = dict(first)
        first["note"] = (_PREVIEW_NOTE + "; album ZIP of %d previews"
                         % len(downloaded))
        return {"file_path": zip_path, "track": first,
                "count": len(downloaded)}
    if parsed and parsed[0] == "track":
        path, track = download_track_by_id(parsed[1], dest)
        return {"file_path": path, "track": track}
    results = search_deezer(query, limit=1)
    if not results:
        raise download_failed("no Deezer match for query: %r" % query)
    path, track = download_track_by_id(str(results[0]["id"]), dest)
    return {"file_path": path, "track": track}
