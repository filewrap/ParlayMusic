"""Provider registry — beetube-bot pattern.

beetube-bot keeps one handler class per source behind a common
`search / list / download` surface and dispatches by provider name.
Parlay's registry is the same idea in miniature: each provider is a
dict of callables; the HTTP layer only knows provider names.

Registered: youtube, spotify, deezer, tiktok, navidrome.
"""
import os

from . import deezer as deezer_mod
from . import navidrome as navidrome_mod
from . import spotify as spotify_mod
from . import spotify_api
from . import tiktok as tiktok_mod
from . import ytdlp as ytdlp_mod
from .util import (MAX_BOT_API_BYTES, bad_request, download_failed)

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
}


def get_provider(name):
    provider = PROVIDERS.get(name)
    if provider is None:
        raise download_failed("unknown provider: %r" % name)
    return provider
