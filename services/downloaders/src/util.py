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


def track_dict(track_id, title, source, artist=None, duration_sec=None,
               url=None, thumbnail=None, note=None):
    t = {"id": str(track_id), "title": title, "source": source}
    if artist:
        t["artist"] = artist
    if duration_sec is not None:
        try:
            t["duration_sec"] = int(duration_sec)
        except (TypeError, ValueError):
            pass
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
