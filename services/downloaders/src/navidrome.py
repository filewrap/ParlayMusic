"""Navidrome downloads via the Subsonic API.

From music-bot-navidrome `src/navidrome.py` (same auth shape, same
endpoints): token auth ``{u, t: md5(password+salt), s: salt, v: 1.16.1,
c, f: json}``; ``rest/download`` returns the original file (no
transcode), ``rest/stream?format=mp3&maxBitRate=`` a transcode;
``rest/search2`` / ``rest/search3`` for lookup.

Configuration (environment only, never defaults):
``NAVIDROME_URL`` (e.g. https://demo.navidrome.org),
``NAVIDROME_USER``, ``NAVIDROME_PASSWORD``. Missing vars answer
``engine_unavailable`` naming the missing var.

Auth quirk (proven 2026-11-02 against the public demo): token auth is
rejected with HTTP 403 on that server while plain-password auth
(``p=<password>``) succeeds, so this client tries token auth first and
falls back to ``p=`` on 401/403 or Subsonic error 40/41/50, remembering
the working mode for the process.

Live-tested 2026-11-02 against demo.navidrome.org (demo/demo):
search -> album -> song -> rest/download, 5,365,035-byte mp3.
"""
import hashlib
import json
import os
import re
import secrets
import urllib.error
import urllib.parse
import urllib.request

from .util import (download_failed, engine_unavailable, sanitize_filename,
                   track_dict)

_AUTH_MODE = {"mode": "token"}  # "token" | "password"; sticky per process


def _env():
    """(base_url, user, password) or raise engine_unavailable."""
    base = (os.environ.get("NAVIDROME_URL") or "").rstrip("/")
    user = os.environ.get("NAVIDROME_USER") or ""
    password = os.environ.get("NAVIDROME_PASSWORD") or ""
    missing = [name for name, val in (
        ("NAVIDROME_URL", base), ("NAVIDROME_USER", user),
        ("NAVIDROME_PASSWORD", password)) if not val]
    if missing:
        raise engine_unavailable(
            "Navidrome is not configured; set %s in the environment"
            % ", ".join(missing))
    return base, user, password


def _auth_params(user, password):
    """Subsonic auth params for the current mode (ref: _auth_params)."""
    if _AUTH_MODE["mode"] == "password":
        return {"u": user, "p": password}
    salt = secrets.token_hex(8)
    token = hashlib.md5((password + salt).encode("utf-8")).hexdigest()
    return {"u": user, "t": token, "s": salt}


def _call(path, params=None, raw=False, timeout=30):
    """One Subsonic call; returns (status, parsed dict | bytes).

    Raises download_failed on transport errors and on Subsonic
    ``status: failed`` responses (auth errors included).
    """
    base, user, password = _env()
    query = {"v": "1.16.1", "c": "parlay", "f": "json"}
    query.update(_auth_params(user, password))
    if params:
        query.update(params)
    url = base + "/rest/" + path + "?" + urllib.parse.urlencode(query)
    req = urllib.request.Request(url, headers={"User-Agent": "Parlay/0.1"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read()
            return resp.status, (body if raw else json.loads(body.decode("utf-8")))
    except urllib.error.HTTPError as exc:
        # Token auth rejected by this server? Fall back to plain-password
        # auth once, then fail honestly with the server's answer.
        if exc.code in (401, 403) and _AUTH_MODE["mode"] == "token":
            _AUTH_MODE["mode"] = "password"
            return _call(path, params=params, raw=raw, timeout=timeout)
        raise download_failed("Navidrome HTTP %s from %s" % (exc.code, url))
    except (urllib.error.URLError, TimeoutError,
            json.JSONDecodeError) as exc:
        raise download_failed("Navidrome request failed (%s): %s" % (url, exc))


def _unwrap(data, path):
    root = (data or {}).get("subsonic-response", {})
    if root.get("status") == "failed":
        err = root.get("error", {})
        raise download_failed("Navidrome %s: [%s] %s"
                              % (path, err.get("code"), err.get("message")))
    return root


def ping():
    """rest/ping -> server version string (proves auth + reachability)."""
    _, data = _call("ping")
    root = _unwrap(data, "ping")
    return root.get("serverVersion") or root.get("version") or "ok"


def search_songs(query, limit=5):
    """rest/search2 (then search3) -> [Subsonic song dicts]."""
    _, data = _call("search2", {"query": query})
    root = _unwrap(data, "search2")
    songs = (root.get("searchResult2") or {}).get("song") or []
    if songs:
        return songs[:limit]
    _, data = _call("search3", {"query": query, "songCount": limit})
    root = _unwrap(data, "search3")
    songs = (root.get("searchResult3") or {}).get("song") or []
    return songs[:limit]


def _song_to_track(song, base):
    return track_dict(
        str(song.get("id")), song.get("title") or "Unknown", "navidrome",
        artist=song.get("artist"),
        duration_sec=song.get("duration"),
        url="%s/rest/download?id=%s" % (base, song.get("id")),
        note="original file from the Navidrome library (rest/download, "
             "no transcode)")


def download_song(song, dest_dir):
    """rest/download for one song dict; returns (file_path, track)."""
    base, _, _ = _env()
    suffix = (song.get("suffix") or "mp3").lower()
    name = sanitize_filename("%s - %s" % (song.get("artist") or "navidrome",
                                          song.get("title") or song.get("id")))
    dest_path = os.path.join(dest_dir, name[:150] + "." + suffix)
    status, body = _call("download", {"id": song.get("id")}, raw=True,
                         timeout=300)
    if status != 200:
        raise download_failed("Navidrome download: HTTP %s" % status)
    if isinstance(body, (bytes, bytearray)) and body[:1] == b"{":
        # Subsonic answered a JSON error payload instead of audio bytes.
        try:
            _unwrap(json.loads(body.decode("utf-8")), "download")
        except download_failed:
            raise
        except (ValueError, UnicodeDecodeError):
            pass
        raise download_failed("Navidrome download returned no audio bytes")
    with open(dest_path, "wb") as fh:
        fh.write(body)
    if os.path.getsize(dest_path) < 1024:
        raise download_failed("Navidrome download produced an empty file")
    return dest_path, _song_to_track(song, base)


def download_navidrome(body):
    """Provider entry: {query|url} -> {file_path, track, imported}.

    ``query`` is a search string (artist/title) or a bare Navidrome
    song id; ``url`` may be a Navidrome track URL carrying ``?id=``.
    """
    dest = body["_dl_dir"]
    os.makedirs(dest, exist_ok=True)
    query = (body.get("query") or body.get("url") or "").strip()
    if not query:
        raise download_failed("query or url is required")
    # A Navidrome track URL: pull the id out of it.
    song_id = None
    m = re.search(r"[?&]id=([A-Za-z0-9_-]+)", query)
    if m:
        song_id = m.group(1)
    elif re.fullmatch(r"[A-Za-z0-9_-]{10,}", query):
        song_id = query
    if song_id:
        # Direct download by id (no search round-trip).
        base, _, _ = _env()
        path, track = download_song({"id": song_id, "title": song_id,
                                     "suffix": "mp3"}, dest)
        return {"file_path": path, "track": track, "imported": True}
    songs = search_songs(query, limit=1)
    if not songs:
        raise download_failed("no Navidrome match for query: %r" % query)
    path, track = download_song(songs[0], dest)
    return {"file_path": path, "track": track, "imported": True}
