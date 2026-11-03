"""Parlay downloaders — shared helpers.

Original code for Parlay. Behaviour ported (not copied) from:
- sanila2007 Telegram-Song-Downloader-Bot (free YouTube song downloads:
  search -> best audio -> mp3)
- xgorn Spotify-Loader (Spotify track/playlist/album -> audio, ZIP or
  one-by-one)
- kodjunkie beetube-bot (provider registry: each source is one handler)
"""
import json
import re
import unicodedata

# 50 MB Bot API cap — see contracts/ipc.json notes. Oversize files are NOT
# failed; they are returned with oversize=true for a later MTProto pass.
MAX_BOT_API_BYTES = 50 * 1024 * 1024

MAX_SEARCH_LIMIT = 20
DEFAULT_SEARCH_LIMIT = 5
MAX_PLAYLIST_TRACKS = 50


class ApiError(Exception):
    """Error carrying a contract error code (contracts/ipc.json)."""

    def __init__(self, code, message, status=400):
        super().__init__(message)
        self.code = code
        self.message = message
        self.status = status


def bad_request(msg):
    return ApiError("bad_request", msg, 400)


def engine_unavailable(msg):
    return ApiError("engine_unavailable", msg, 503)


def download_failed(msg):
    return ApiError("download_failed", msg, 502)


def sanitize_filename(name, max_len=180):
    """Make a string safe as a single path component.

    Drops path separators, control chars and '..' segments; never returns
    an absolute path or an empty string.
    """
    name = unicodedata.normalize("NFKD", str(name))
    name = "".join(c for c in name if c.isprintable())
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", name)
    name = name.strip(" .")
    name = re.sub(r"\.+", ".", name)
    if not name or name in (".", ".."):
        name = "track"
    if len(name) > max_len:
        name = name[:max_len].rstrip(" .")
    return name


def safe_join(base, *parts):
    """Join parts under base, refusing any escape (no .., no absolute)."""
    import os

    base = os.path.realpath(base)
    path = os.path.realpath(os.path.join(base, *[sanitize_filename(p) for p in parts]))
    if os.path.commonpath([base, path]) != base:
        raise bad_request("unsafe path")
    return path


def format_duration_str(sec, is_live=False):
    """IPC-3 (§29): human duration — "M:SS" under an hour, "Xh Ym Zs"
    at/over an hour, "∞" when the duration is unknown (sec <= 0) or
    the track is live. Zero-valued hour/minute/second parts are dropped.
    Same rendering as vc-go / vc-ts / vc-rust (asserted identically
    in each service's tests)."""
    try:
        sec = int(sec)
    except (TypeError, ValueError):
        sec = 0
    if is_live or sec <= 0:
        return "∞"
    if sec < 3600:
        return "%d:%02d" % (sec // 60, sec % 60)
    parts = ["%dh" % (sec // 3600)]
    if (sec % 3600) // 60:
        parts.append("%dm" % ((sec % 3600) // 60))
    if sec % 60:
        parts.append("%ds" % (sec % 60))
    return " ".join(parts)


def format_upload_date(raw):
    """IPC-3: yt-dlp upload_date ("YYYYMMDD") -> "YYYY-MM-DD" for the
    search metadata rows. Anything else passes through unchanged."""
    raw = str(raw or "")
    if len(raw) == 8 and raw.isdigit():
        return "%s-%s-%s" % (raw[:4], raw[4:6], raw[6:])
    return raw


def track_dict(track_id, title, source, artist=None, duration_sec=None,
               url=None, thumbnail=None, note=None, views=None,
               uploader=None, upload_time=None, is_live=False):
    t = {"id": str(track_id), "title": title, "source": source}
    if artist:
        t["artist"] = artist
    if duration_sec is not None:
        try:
            t["duration_sec"] = int(duration_sec)
        except (TypeError, ValueError):
            pass
    # IPC-3 (§29): duration_str is always derived (never invented —
    # unknown durations render "∞"); views/uploader/upload_time only
    # when the source reported them; is_live only when true.
    t["duration_str"] = format_duration_str(
        t.get("duration_sec", 0), is_live)
    if views is not None:
        try:
            views = int(views)
        except (TypeError, ValueError):
            views = None
        if views is not None and views > 0:
            t["views"] = views
    if uploader:
        t["uploader"] = uploader
    if upload_time:
        t["upload_time"] = format_upload_date(upload_time)
    if is_live:
        t["is_live"] = True
    if url:
        t["url"] = url
    if thumbnail:
        t["thumbnail"] = thumbnail
    if note:
        t["note"] = note
    return t


def ok_envelope(data):
    return {"ok": True, "data": data}


def err_envelope(err):
    return {"ok": False, "error": {"code": err.code, "message": err.message}}


def parse_duration(value):
    """Parse 'mm:ss', 'hh:mm:ss' or a raw second count into int seconds."""
    if value is None:
        return None
    if isinstance(value, (int, float)):
        return int(value)
    parts = str(value).strip().split(":")
    try:
        nums = [int(p) for p in parts]
    except ValueError:
        return None
    total = 0
    for n in nums:
        total = total * 60 + n
    return total
