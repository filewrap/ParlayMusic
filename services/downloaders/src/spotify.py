"""Spotify resolution without Spotify API credentials.

Spotify-Loader (xgorn) drives `spotdl` for everything: it hands spotdl
the URL/query and spotdl resolves Spotify metadata, finds a YouTube
match and tags the file. This module re-implements that *flow* against
Parlay's contract:

1. Parse the Spotify URL (track / album / playlist / artist, web or
   spotify: URI).
2. Resolve metadata: with SPOTIFY_CLIENT_ID/SECRET, the real Web API
   (spotify_api); without them, Spotify's public embed page
   (__NEXT_DATA__ track list / track entity — real titles/artists/
   durations, no auth), then yt-dlp's flat Spotify extractor (public
   pages, no login — now refused: yt-dlp answers "[DRM]" for Spotify
   URLs, proven 2026-11-02), then Spotify's public oEmbed endpoint
   (no auth) for a single track's title/artist.
3. Download the audio by searching YouTube for "<artist> <title>"
   (the same fallback Spotify-Loader gets with spotdl --use-youtube),
   or via spotdl itself when it is installed (optional, see
   requirements.txt).
4. Playlist/album: resolve the track list, download each, return ZIP
   (mode=zip, like Spotify-Loader's "Send as ZIP") or the file list
   (mode=files, "Send one by one").

No Spotify client id/secret is needed or used anywhere here.
"""
import json
import os
import re
import shutil
import subprocess
import urllib.parse
import urllib.request
import uuid
import zipfile

from . import spotify_api
from .util import (MAX_PLAYLIST_TRACKS, download_failed, parse_duration,
                   sanitize_filename, track_dict)
from .ytdlp import download_youtube_audio, extract_info_flat

_SPOTIFY_RE = re.compile(
    r"(?:https?://open\.spotify\.com/|spotify:)"
    r"(track|album|playlist|artist)[/:]([A-Za-z0-9]+)")

_OEMBED = "https://open.spotify.com/oembed?url=%s"

_SPOTDL = shutil.which("spotdl")

# Spotify's public embed page (open.spotify.com/embed/<kind>/<id>) ships
# its track list / track metadata inside a __NEXT_DATA__ JSON blob — no
# auth, no API credentials. This is the no-creds fallback now that
# yt-dlp refuses Spotify URLs outright ("[DRM] The requested site is
# known to use DRM protection", proven 2026-11-02): playlist/album
# track lists come from entity.trackList (uri/title/subtitle/duration
# in ms), single tracks from the entity itself (title/artists/duration
# ms + a 30 s audio preview URL).
_EMBED_UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
             "AppleWebKit/537.36 (KHTML, like Gecko) "
             "Chrome/145.0.0.0 Safari/537.36")
_NEXT_DATA_RE = re.compile(
    r'<script id="__NEXT_DATA__"[^>]*>(.*?)</script>', re.S)

_EMBED_NOTE = ("resolved via Spotify's public embed page "
               "(no API credentials)")


def _entity_from_html(html):
    """Extract the embed page's entity dict from raw HTML (pure)."""
    m = _NEXT_DATA_RE.search(html)
    if not m:
        return None
    try:
        data = json.loads(m.group(1))
        return data["props"]["pageProps"]["state"]["data"]["entity"]
    except (ValueError, KeyError, TypeError):
        return None


def _embed_entity(kind, sid):
    """Fetch the embed page's entity dict, or None on any failure."""
    url = ("https://open.spotify.com/embed/%s/%s?utm_source=generator"
           % (kind, sid))
    req = urllib.request.Request(url, headers={"User-Agent": _EMBED_UA})
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            html = resp.read().decode("utf-8", errors="replace")
    except Exception:
        return None
    return _entity_from_html(html)


def _embed_track(kind, sid):
    """One track's real metadata from its embed page, or None."""
    if kind != "track":
        return None
    ent = _embed_entity(kind, sid)
    if not ent or not ent.get("title"):
        return None
    artists = ", ".join(a.get("name", "") for a in ent.get("artists") or []
                        if a.get("name"))
    track = track_dict(
        sid, ent.get("title") or "", "spotify", artist=artists,
        duration_sec=parse_duration((ent.get("duration") or 0) // 1000),
        url="https://open.spotify.com/track/%s" % sid,
        note=_EMBED_NOTE)
    preview = (ent.get("audioPreview") or {}).get("url")
    if preview:
        track["preview_url"] = preview
    return track


def _embed_track_list(kind, sid):
    """Playlist/album track list from the embed page, or None."""
    ent = _embed_entity(kind, sid)
    if not ent:
        return None
    items = ent.get("trackList") or []
    tracks = []
    for item in items:
        uri = item.get("uri") or ""
        tid = uri.rsplit(":", 1)[-1] if uri.startswith("spotify:track:") else ""
        if not tid or not item.get("title"):
            continue
        tracks.append(track_dict(
            tid, item.get("title") or "", "spotify",
            artist=item.get("subtitle") or "",
            duration_sec=parse_duration((item.get("duration") or 0) // 1000),
            url="https://open.spotify.com/track/%s" % tid,
            note=_EMBED_NOTE))
    return tracks or None

# A Spotify track is a song: never match it to a multi-hour YouTube
# compilation (e.g. a playlist-title search matching a 3h "chill hits"
# video). Prefer the first YouTube match at or under this length.
MAX_SPOTIFY_MATCH_DURATION_S = int(
    os.environ.get("PARLAY_SPOTIFY_MAX_MATCH_SEC", "1200"))


def _pick_match(query):
    """Best YouTube match for a Spotify track: first result whose
    duration is a song-length (or unknown). If every match is a
    multi-hour compilation or a livestream, fail fast instead of
    downloading hundreds of MB of the wrong thing."""
    from .ytdlp import search_youtube

    results = search_youtube(query, limit=5)
    if not results:
        raise download_failed("no YouTube match for query: %r" % query)
    for t in results:
        if t.get("live"):
            continue
        dur = t.get("duration_sec")
        if dur is None or dur <= MAX_SPOTIFY_MATCH_DURATION_S:
            return t
    raise download_failed(
        "no song-length YouTube match for %r: all %d matches are "
        "long compilations/livestreams (shortest %ss)"
        % (query, len(results),
           min(r["duration_sec"] for r in results
               if r.get("duration_sec") is not None)))


def parse_spotify_url(url):
    """Return (kind, spotify_id) or None if this is not a Spotify link."""
    if not isinstance(url, str):
        return None
    m = _SPOTIFY_RE.search(url.strip())
    if not m:
        return None
    return m.group(1), m.group(2)


def _oembed_title(url):
    """Public oEmbed lookup: returns (title, author) or (None, None)."""
    try:
        req = urllib.request.Request(
            _OEMBED % urllib.parse.quote(url, safe=""),
            headers={"User-Agent": "Parlay/0.1"})
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.loads(resp.read().decode("utf-8"))
        return data.get("title"), data.get("author_name")
    except Exception:
        return None, None


def resolve_track(url):
    """Resolve one Spotify track URL to a Track dict (source=spotify).

    With SPOTIFY_CLIENT_ID/SECRET configured the metadata comes from
    the real Spotify Web API (spotify_api.get_track); without them the
    old fallback applies (yt-dlp flat extract, then public oEmbed) and
    the track carries a `note` saying so.
    """
    parsed = parse_spotify_url(url)
    if not parsed or parsed[0] != "track":
        raise download_failed("not a Spotify track URL: %r" % url)
    if spotify_api.configured():
        track = spotify_api.get_track(parsed[1])
        if track is not None:
            track["note"] = "resolved via Spotify Web API (client credentials)"
            return track
        # API configured but the call failed: fall through to the
        # public fallback and say so, instead of failing outright.
        note = ("Spotify Web API track fetch failed; resolved without "
                "API credentials fallback (YouTube/oEmbed)")
    else:
        note = ("resolved without Spotify API credentials; audio matched "
                "via YouTube search")
    embedded = _embed_track("track", parsed[1])
    if embedded is not None:
        return embedded
    info = extract_info_flat(url)
    title = artist = None
    if info:
        title = info.get("title")
        artist = info.get("uploader") or info.get("artist")
    if not title:
        title, artist = _oembed_title(url)
    if not title:
        raise download_failed(
            "cannot resolve Spotify track metadata (no Spotify API "
            "credentials and oEmbed/yt-dlp both failed)")
    return track_dict(parsed[1], title, "spotify", artist=artist,
                      url=url, note=note)


def resolve_collection(url):
    """Resolve a playlist/album/artist URL to a list of Track dicts.

    With SPOTIFY_CLIENT_ID/SECRET configured the track list comes from
    the real Spotify Web API (spotify_api); without them the old
    fallback applies (yt-dlp flat extract of the public page, then the
    page title via oEmbed) and every track carries a `note` saying so.
    """
    parsed = parse_spotify_url(url)
    if not parsed or parsed[0] not in ("playlist", "album", "artist"):
        raise download_failed(
            "not a Spotify playlist/album/artist URL: %r" % url)
    kind, sid = parsed
    if spotify_api.configured():
        api_tracks = None
        api_error = None
        try:
            if kind == "album":
                got = spotify_api.get_album(sid)
                api_tracks = got[1] if got else None
            elif kind == "playlist":
                got = spotify_api.get_playlist(sid)
                api_tracks = got[1] if got else None
            else:
                got = spotify_api.get_artist_top(sid)
                api_tracks = got[1] if got else None
        except RuntimeError as exc:
            api_error = str(exc)
        if api_tracks:
            for t in api_tracks:
                t["note"] = ("resolved via Spotify Web API "
                             "(client credentials)")
            return api_tracks
        note = ("Spotify Web API %s fetch failed%s; track list via "
                "yt-dlp/oEmbed fallback"
                % (kind, (": " + api_error) if api_error else ""))
    else:
        note = ("resolved without Spotify API credentials; audio matched "
                "via YouTube search")
    embedded = _embed_track_list(kind, sid)
    if embedded:
        return embedded
    info = extract_info_flat(url)
    tracks = []
    if info:
        entries = info.get("entries") or []
        for e in entries:
            if not e:
                continue
            title = e.get("title")
            if not title:
                continue
            tracks.append(track_dict(
                e.get("id") or title, title, "spotify",
                artist=e.get("uploader") or e.get("artist"),
                duration_sec=parse_duration(e.get("duration")),
                url=url, note=note))
    if not tracks:
        # Playlist pages that yt-dlp cannot list: fall back to the page
        # title as a single searchable item so the caller still gets a
        # usable result instead of a bare failure.
        title, artist = _oembed_title(url)
        if title:
            tracks.append(track_dict(sid, title, "spotify", artist=artist,
                                     url=url, note=note + "; track list "
                                     "unavailable, page title used"))
    if not tracks:
        raise download_failed(
            "cannot resolve Spotify %s track list (no Spotify API "
            "credentials and yt-dlp/oEmbed both failed)" % kind)
    return tracks


def _spotdl_available():
    return bool(_SPOTDL)


def download_spotify_track(track, dest_dir):
    """Download one resolved Spotify track; returns (file_path, track)."""
    query = "%s %s" % (track.get("artist") or "", track.get("title") or "")
    query = query.strip()
    if not query:
        raise download_failed("empty Spotify track title")
    if _spotdl_available():
        return _download_via_spotdl(track, dest_dir)
    match = _pick_match(query)
    file_path, yt_track = download_youtube_audio(match["url"], dest_dir)
    merged = dict(track)
    merged["url"] = yt_track.get("url") or merged.get("url")
    if yt_track.get("duration_sec") is not None:
        merged["duration_sec"] = yt_track["duration_sec"]
    if yt_track.get("thumbnail"):
        merged["thumbnail"] = yt_track["thumbnail"]
    return file_path, merged


def _download_via_spotdl(track, dest_dir):
    """Optional path: spotdl installed (see requirements.txt)."""
    job_dir = os.path.join(dest_dir, "job-" + uuid.uuid4().hex[:12])
    os.makedirs(job_dir, exist_ok=True)
    query = "%s %s" % (track.get("artist") or "", track.get("title") or "")
    cmd = [_SPOTDL, query.strip(), "--output-format", "mp3",
           "--use-youtube",
           "--path-template",
           os.path.join(job_dir, "{artist} - {title}.{ext}")]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        raise download_failed("spotdl timed out")
    files = [os.path.join(job_dir, f) for f in os.listdir(job_dir)
             if os.path.isfile(os.path.join(job_dir, f))]
    if proc.returncode != 0 or not files:
        raise download_failed(
            "spotdl failed: %s" % (proc.stderr.strip() or "no file produced"))
    return files[0], track


def download_spotify_collection(url, dest_dir, mode="zip"):
    """Spotify-Loader flow: playlist/album -> ZIP or one-by-one files.

    Returns (file_path_or_None, tracks, downloaded_files, truncated).
    mode=zip -> a single .zip path; mode=files -> None + per-file list.
    """
    tracks = resolve_collection(url)
    truncated = False
    if len(tracks) > MAX_PLAYLIST_TRACKS:
        tracks = tracks[:MAX_PLAYLIST_TRACKS]
        truncated = True
    downloaded = []
    first_error = None
    for track in tracks:
        try:
            path, _ = download_spotify_track(track, dest_dir)
            downloaded.append((track, path))
        except Exception as exc:
            # One bad track must not kill the whole playlist (same
            # behaviour as Spotify-Loader's per-file loop).
            if first_error is None:
                first_error = str(exc)
            continue
    if not downloaded:
        raise download_failed(
            "no tracks could be downloaded from %r%s"
            % (url, (": " + first_error) if first_error else ""))
    if mode == "zip":
        first = downloaded[0][0]
        artist = sanitize_filename(first.get("artist") or "spotify")
        zip_name = "%s-playlist-%s.zip" % (artist, uuid.uuid4().hex[:8])
        zip_path = os.path.join(dest_dir, zip_name)
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
            for track, path in downloaded:
                arc = sanitize_filename(
                    "%s - %s.mp3" % (track.get("artist") or "unknown",
                                     track.get("title") or "track"))
                zf.write(path, arcname=arc)
        return zip_path, [t for t, _ in downloaded], [p for _, p in downloaded], truncated
    return None, [t for t, _ in downloaded], [p for _, p in downloaded], truncated
