"""Provider registry — beetube-bot pattern.

beetube-bot keeps one handler class per source behind a common
`search / list / download` surface and dispatches by provider name.
Parlay's registry is the same idea in miniature: each provider is a
dict of callables; the HTTP layer only knows provider names.

Registered: youtube, spotify, deezer, tiktok, navidrome, soulseek.
"""
import os

from . import deezer as deezer_mod
from . import navidrome as navidrome_mod
from . import spotify as spotify_mod
from . import spotify_api
from . import tiktok as tiktok_mod
from . import ytdlp as ytdlp_mod
from .util import (MAX_BOT_API_BYTES, MAX_PLAYLIST_TRACKS, bad_request,
                   download_failed, engine_unavailable)

OVERSIZE_LIMIT = MAX_BOT_API_BYTES


def _file_result(file_path, track):
    size = os.path.getsize(file_path)
    return {"file_path": file_path, "track": track,
            "size_bytes": size, "oversize": size > OVERSIZE_LIMIT}


def youtube_download(body):
    query = (body.get("query") or "").strip()
    if not query:
        raise bad_request("query is required")
    dest = body["_dl_dir"]
    file_path, track = ytdlp_mod.download_youtube_audio(query, dest)
    return _file_result(file_path, track)


def spotify_download(body):
    url = (body.get("url") or "").strip()
    if not url:
        raise bad_request("url is required")
    if not spotify_mod.parse_spotify_url(url):
        raise bad_request("not a Spotify URL: %r" % url)
    kind, _ = spotify_mod.parse_spotify_url(url)
    if kind != "track":
        raise bad_request(
            "use /v1/download/spotify-playlist for %s URLs" % kind)
    dest = body["_dl_dir"]
    track = spotify_mod.resolve_track(url)
    file_path, track = spotify_mod.download_spotify_track(track, dest)
    return _file_result(file_path, track)


def spotify_playlist_download(body):
    url = (body.get("url") or "").strip()
    if not url:
        raise bad_request("url is required")
    mode = (body.get("mode") or "zip").strip().lower()
    if mode not in ("zip", "files"):
        raise bad_request("mode must be zip or files")
    if not spotify_mod.parse_spotify_url(url):
        raise bad_request("not a Spotify URL: %r" % url)
    kind, _ = spotify_mod.parse_spotify_url(url)
    if kind == "track":
        raise bad_request(
            "use /v1/download/spotify for single track URLs")
    dest = body["_dl_dir"]
    zip_path, tracks, files, truncated = \
        spotify_mod.download_spotify_collection(url, dest, mode)
    data = {"count": len(files), "tracks": tracks,
            "truncated": truncated,
            "truncated_to": len(tracks) if truncated else None}
    if mode == "zip":
        data["file_path"] = zip_path
        data["size_bytes"] = os.path.getsize(zip_path)
        data["oversize"] = data["size_bytes"] > OVERSIZE_LIMIT
    else:
        data["file_paths"] = files
        data["size_bytes"] = sum(os.path.getsize(p) for p in files)
        data["oversize"] = data["size_bytes"] > OVERSIZE_LIMIT
    return data


def spotify_collection(body):
    """POST /v1/spotify/collection — contract `spotify.collection`.

    Metadata only: no files are downloaded. Returns {api, kind, name,
    tracks, truncated}. With SPOTIFY_CLIENT_ID/SECRET the list comes
    from the real Spotify Web API (same calls as resolve_collection);
    without them the same yt-dlp/oEmbed fallback applies and `api`
    says so honestly. Track lists are capped at MAX_PLAYLIST_TRACKS
    (50), same as the download path.
    """
    url = (body.get("url") or "").strip()
    if not url:
        raise bad_request("url is required")
    parsed = spotify_mod.parse_spotify_url(url)
    if not parsed:
        raise bad_request("not a Spotify URL: %r" % url)
    kind, sid = parsed
    if kind == "track":
        raise bad_request(
            "use /v1/download/spotify for single track URLs")
    if spotify_api.configured():
        if kind == "album":
            got = spotify_api.get_album(sid)
            if not got:
                raise download_failed(
                    "Spotify API could not resolve album %s" % sid)
            name, tracks, total = got
            truncated = total > len(tracks)
        elif kind == "playlist":
            got = spotify_api.get_playlist(sid)
            if not got:
                raise download_failed(
                    "Spotify API could not resolve playlist %s" % sid)
            name, tracks, _total, truncated = got
        else:  # artist
            got = spotify_api.get_artist_top(sid)
            if not got:
                raise download_failed(
                    "Spotify API could not resolve artist %s" % sid)
            name, tracks = got
            truncated = False
        api = "spotify-web-api"
    else:
        tracks = spotify_mod.resolve_collection(url)
        title, _author = spotify_mod._oembed_title(url)
        name = title or ""
        api = "yt-dlp/oEmbed fallback"
        truncated = False
    if len(tracks) > MAX_PLAYLIST_TRACKS:
        tracks = tracks[:MAX_PLAYLIST_TRACKS]
        truncated = True
    return {"api": api, "kind": kind, "name": name,
            "tracks": tracks, "truncated": truncated}


def deezer_download(body):
    result = deezer_mod.download_deezer(body)
    if "count" in result:
        # Album input: ZIP of previews.
        size = os.path.getsize(result["file_path"])
        return {"file_path": result["file_path"], "track": result["track"],
                "count": result["count"],
                "size_bytes": size, "oversize": size > OVERSIZE_LIMIT}
    return _file_result(result["file_path"], result["track"])


def tiktok_download(body):
    url = (body.get("url") or body.get("query") or "").strip()
    if not url:
        raise bad_request("url is required")
    dest = body["_dl_dir"]
    result = tiktok_mod.download_tiktok({**body, "url": url, "_dl_dir": dest})
    return _file_result(result["file_path"], result["track"])


def navidrome_download(body):
    result = navidrome_mod.download_navidrome(body)
    data = _file_result(result["file_path"], result["track"])
    data["imported"] = result.get("imported", True)
    return data


def soulseek_download(body):
    """Soulseek via slskd (see src/soulseek/).

    Body: {query} or {artist, title, duration_sec?} for one track, or
    {album: {artist, title, tracks: [...]}} for a whole album.
    `accept_lossy: true` allows mp3>=256kbps / m4a when no FLAC exists.
    A slskd that is down or not logged in answers `engine_unavailable`
    naming SLSKD_URL — a search that ran and found nothing is a
    `download_failed` ("nothing found on Soulseek"), never an
    engine error.
    """
    from .soulseek import client as slskd_client
    from .soulseek import downloader as slskd_dl

    if not slskd_client.slskd_available():
        raise engine_unavailable(
            "slskd is unreachable at %s (SLSKD_URL / SLSKD_API_KEY); "
            "Soulseek downloads need a running slskd daemon"
            % slskd_client._base_url())
    dest = body["_dl_dir"]
    accept_lossy = bool(body.get("accept_lossy"))
    album = body.get("album")
    if isinstance(album, dict) and album.get("tracks"):
        result = slskd_dl.download_album(album, dest)
        if not result["downloaded"]:
            raise download_failed(
                "Soulseek album download found nothing usable: %s"
                % (result["failed"][0][1] if result["failed"] else "no matches"))
        data = {"count": result["downloaded"], "total": result["total"],
                "failed": result["failed"],
                "format": result["format"],
                "source_counts": result["source_counts"],
                "album_dir": result["album_dir"],
                "source": "soulseek"}
        return data
    artist = (body.get("artist") or "").strip()
    title = (body.get("title") or "").strip()
    query = (body.get("query") or "").strip()
    if not title and query:
        # "Artist - Title" or bare title.
        if " - " in query:
            artist, title = (p.strip() for p in query.split(" - ", 1))
        else:
            title = query
    if not title:
        raise bad_request("artist+title or query is required")
    track = {"title": title, "artist": artist}
    duration = body.get("duration_sec")
    if duration:
        try:
            track["duration_sec"] = int(duration)
        except (TypeError, ValueError):
            pass
    try:
        path, _downloaded, fmt, peer = slskd_dl.download_single_track(
            track, dest, accept_lossy=accept_lossy)
    except slskd_client.SearchError as exc:
        raise engine_unavailable("Soulseek search could not run: %s" % exc)
    track_out = {"id": "soulseek:%s - %s" % (artist, title),
                 "title": title, "source": "soulseek", "artist": artist,
                 "note": "full track from Soulseek peer %s (%s)" % (peer, fmt)}
    if duration:
        track_out["duration_sec"] = track.get("duration_sec")
    data = _file_result(path, track_out)
    data["peer"] = peer
    data["format"] = fmt
    return data


def search(body):
    query = (body.get("query") or "").strip()
    if not query:
        raise bad_request("query is required")
    source = (body.get("source") or "all").strip().lower()
    if source not in ("youtube", "spotify", "deezer", "all"):
        raise bad_request("source must be youtube, spotify, deezer or all")
    try:
        limit = int(body.get("limit") or 5)
    except (TypeError, ValueError):
        raise bad_request("limit must be an integer")
    limit = max(1, min(limit, 20))

    results = []
    if source in ("youtube", "all"):
        results += ytdlp_mod.search_youtube(query, limit)
    if source in ("spotify", "all"):
        if spotify_api.configured():
            # Real Spotify search when credentials exist.
            api_results = spotify_api.search(query, limit)
            if api_results:
                for t in api_results:
                    t = dict(t)
                    t["note"] = "resolved via Spotify Web API (client credentials)"
                    results.append(t)
            else:
                results += _spotify_fallback_search(query, limit)
        else:
            results += _spotify_fallback_search(query, limit)
    if source in ("deezer", "all"):
        try:
            results += deezer_mod.search_deezer(query, limit)
        except Exception:
            # Deezer search is best-effort inside a multi-source query.
            pass
    return {"results": results[:limit] if source != "all" else results}


def _spotify_fallback_search(query, limit):
    """No Spotify API credentials: match via YouTube and mark the
    source honestly, with a note on each track."""
    note = ("spotify match via YouTube search; no Spotify API "
            "credentials configured")
    out = []
    for t in ytdlp_mod.search_youtube(query, limit):
        t = dict(t)
        t["source"] = "spotify"
        t["note"] = note
        out.append(t)
    return out


# name -> {"download": fn, "search": fn|None}
PROVIDERS = {
    "youtube": {"download": youtube_download, "search": search},
    "spotify": {"download": spotify_download, "search": search},
    "deezer": {"download": deezer_download, "search": search},
    "tiktok": {"download": tiktok_download, "search": None},
    "navidrome": {"download": navidrome_download, "search": None},
    "soulseek": {"download": soulseek_download, "search": None},
}


def get_provider(name):
    provider = PROVIDERS.get(name)
    if provider is None:
        raise download_failed("unknown provider: %r" % name)
    return provider
