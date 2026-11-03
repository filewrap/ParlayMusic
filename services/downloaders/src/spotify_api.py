"""Real Spotify Web API client (client-credentials flow).

Mirrors services/spotify-rust/src/main.rs (same flow, same endpoints):
``POST https://accounts.spotify.com/api/token`` with HTTP Basic auth
(``SPOTIFY_CLIENT_ID:SPOTIFY_CLIENT_SECRET``) and
``grant_type=client_credentials``, then ``GET https://api.spotify.com/v1/``
with a Bearer token. Stdlib only (urllib).

When the credentials are absent every function returns None / False and
callers fall back to the yt-dlp/oEmbed path in spotify.py — a track list
is never fabricated. The app token is cached in memory only, never
written to disk.
"""
import base64
import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request

from .util import format_duration_str

_TOKEN_URL = "https://accounts.spotify.com/api/token"
_API_BASE = "https://api.spotify.com/v1"

_token_cache = {"token": None, "expiry": 0}


def configured():
    """True only when both credential env vars are present and non-empty."""
    return bool(os.environ.get("SPOTIFY_CLIENT_ID")
                and os.environ.get("SPOTIFY_CLIENT_SECRET"))


def _http_json(url, headers=None, data=None, timeout=20):
    req = urllib.request.Request(
        url, data=data,
        headers={"User-Agent": "Parlay/0.1", **(headers or {})},
        method="POST" if data is not None else "GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        raise RuntimeError("Spotify HTTP %s from %s" % (exc.code, url))
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
        raise RuntimeError("Spotify request failed (%s): %s" % (url, exc))


def get_app_token():
    """Client-credentials access token (cached until ~60s before expiry)."""
    now = time.time()
    if _token_cache["token"] and _token_cache["expiry"] > now + 60:
        return _token_cache["token"]
    client_id = os.environ.get("SPOTIFY_CLIENT_ID") or ""
    secret = os.environ.get("SPOTIFY_CLIENT_SECRET") or ""
    if not client_id or not secret:
        return None
    basic = base64.b64encode(
        ("%s:%s" % (client_id, secret)).encode("utf-8")).decode("ascii")
    data = urllib.parse.urlencode(
        {"grant_type": "client_credentials"}).encode("utf-8")
    payload = _http_json(_TOKEN_URL, headers={
        "Authorization": "Basic " + basic,
        "Content-Type": "application/x-www-form-urlencoded"}, data=data)
    token = payload.get("access_token")
    if not token:
        raise RuntimeError("Spotify token endpoint returned no access_token")
    _token_cache["token"] = token
    _token_cache["expiry"] = now + int(payload.get("expires_in") or 3600)
    return token


def api_get(path):
    """GET one Web API resource; raises RuntimeError on any failure."""
    token = get_app_token()
    if not token:
        raise RuntimeError("no Spotify credentials configured")
    return _http_json(_API_BASE + path,
                      headers={"Authorization": "Bearer " + token})


def _artists(track):
    return ", ".join(a.get("name", "") for a in track.get("artists") or []
                     if a.get("name"))


def _track_dict(item):
    """Spotify track object -> contract Track dict (+ album/thumbnail)."""
    album = item.get("album") or {}
    images = album.get("images") or []
    duration_sec = int((item.get("duration_ms") or 0) // 1000)
    track = {
        "id": str(item.get("id") or ""),
        "title": item.get("name") or "",
        "source": "spotify",
        "artist": _artists(item),
        "duration_sec": duration_sec,
        # IPC-3 (§29): derived human duration; a Spotify track is
        # never live, and the API reports no views/uploader/upload
        # date (those stay absent, never invented).
        "duration_str": format_duration_str(duration_sec, False),
        "url": (item.get("external_urls") or {}).get("spotify") or "",
        "album": album.get("name") or "",
    }
    if images:
        track["thumbnail"] = images[-1].get("url")
    return track


def get_track(track_id):
    """One track's real metadata, or None if the API call fails."""
    try:
        return _track_dict(api_get("/tracks/%s" % track_id))
    except RuntimeError:
        return None


def get_album(album_id):
    """(name, [Track], total) for an album, or None on API failure."""
    try:
        data = api_get("/albums/%s" % album_id)
    except RuntimeError:
        return None
    items = ((data.get("tracks") or {}).get("items") or [])
    tracks = []
    for item in items:
        item = dict(item)
        item.setdefault("album", {"name": data.get("name"),
                                  "images": data.get("images") or []})
        tracks.append(_track_dict(item))
    total = int(((data.get("tracks") or {}).get("total")) or len(tracks))
    return data.get("name") or "", tracks, total


def get_playlist(playlist_id, limit=100):
    """(name, [Track], total, truncated) for a playlist (paginated)."""
    try:
        meta = api_get("/playlists/%s" % playlist_id)
    except RuntimeError:
        return None
    name = meta.get("name") or ""
    total = int(((meta.get("tracks") or {}).get("total")) or 0)
    tracks = []
    offset = 0
    while len(tracks) < min(total, limit):
        try:
            page = api_get("/playlists/%s/tracks?limit=100&offset=%d"
                           % (playlist_id, offset))
        except RuntimeError:
            break
        items = page.get("items") or []
        if not items:
            break
        for item in items:
            track = (item or {}).get("track")
            if track:
                tracks.append(_track_dict(track))
        offset += len(items)
        if len(items) < 100:
            break
    return name, tracks, total, total > limit


def get_artist_top(artist_id):
    """(name, [Track]) — the artist's top tracks (max 10, market US)."""
    try:
        artist = api_get("/artists/%s" % artist_id)
        top = api_get("/artists/%s/top-tracks?market=US" % artist_id)
    except RuntimeError:
        return None
    return artist.get("name") or "", [_track_dict(t) for t in top.get("tracks") or []]


def search(query, limit=5):
    """Web API track search -> [Track] (real Spotify search)."""
    try:
        data = api_get("/search?%s" % urllib.parse.urlencode(
            {"q": query, "type": "track", "limit": max(1, min(limit, 20))}))
    except RuntimeError:
        return None
    items = ((data.get("tracks") or {}).get("items") or [])
    return [_track_dict(t) for t in items]
