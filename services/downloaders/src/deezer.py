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

Full tracks with DEEZER_ARL (Phase 5):
When the `DEEZER_ARL` env var holds a Deezer `arl` session cookie
(the 192-char cookie from a logged-in browser session — name only in
docs, never a value), the full-track path runs BEFORE yt-dlp/preview
(deemix algorithm, re-implemented from the public deemix source):
  1. GET https://www.deezer.com/ with `Cookie: arl=...` -> scrape the
     `api_token` from the page.
  2. gw-light `song.getData` (POST ajax/gw-light.php) -> MD5_ORIGIN,
     TRACK_TOKEN, MEDIA_VERSION, FILESIZE_*.
  3. Modern URL: gw-light `deezer.getUserData` -> OPTIONS.license_token,
     then POST https://media.deezer.com/v1/get_url {license_token,
     track_tokens:[TRACK_TOKEN], format} -> CDN URL.
  4. Legacy fallback: AES-ECB-encrypted stream path (key
     'jo6aey6haid2Teih') -> https://e-cdns-proxy-<md5[0]>.dzcdn.net/
     mobile/1/<hex>.
  5. The CDN bytes are Blowfish-CBC-encrypted (key derived from the
     track id, IV 00 01 .. 07); every 2048-byte block at the head of
     each 6144-byte chunk is decrypted while streaming.
Format: DEEZER_FORMAT (MP3_320 default, MP3_128, FLAC); a free
account only gets MP3_128 — the code falls back down the ladder
(FLAC -> MP3_320 -> MP3_128) based on FILESIZE_* availability.
Without the ARL, or when the ARL path fails for any reason, the
preview path runs exactly as before and the track note says why.
pycryptodomex (AES + Blowfish) is required for the ARL path; without
it the ARL attempt fails honestly and previews are used instead.
"""
import hashlib
import json
import os
import re
import subprocess
import sys
import urllib.parse
import urllib.request
import uuid
import zipfile

try:
    from Cryptodome.Cipher import AES as _AES, Blowfish as _Blowfish
except Exception:  # pycryptodomex not installed: ARL path unavailable
    _AES = None
    _Blowfish = None

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


# --------------------------------------------------- ARL full tracks ---
# Deezer full-track download with an `arl` session cookie (Phase 5).
# Algorithm re-implemented from the public deemix source
# (deemix/utils/crypto.py, deemix/decryption.py, deemix/downloader.py):
# gw-light song.getData -> media.deezer.com get_url (modern) or the
# AES-ECB legacy stream URL, then Blowfish-CBC chunk decryption.
# No cookie value is ever logged or stored; DEEZER_ARL is read from
# the environment at call time only.

_ARL_ENV = "DEEZER_ARL"
_FORMAT_ENV = "DEEZER_FORMAT"
# deemix TrackFormats numbers.
_FORMAT_NUMBERS = {"MP3_128": 1, "MP3_320": 3, "FLAC": 9}
_FORMAT_EXT = {"MP3_128": ".mp3", "MP3_320": ".mp3", "FLAC": ".flac"}
# AES-ECB key for the legacy stream-path encryption (deemix constant).
_LEGACY_URL_KEY = b"jo6aey6haid2Teih"
# Blowfish key-derivation secret (deemix constant).
_BLOWFISH_SECRET = "g4el58wc0zvf9na1"
_BLOWFISH_IV = bytes([0, 1, 2, 3, 4, 5, 6, 7])
_GW_LIGHT = "https://www.deezer.com/ajax/gw-light.php"


def arl_configured():
    """True when a Deezer ARL cookie is present in the environment."""
    return bool(os.environ.get(_ARL_ENV))


def _arl():
    return os.environ.get(_ARL_ENV) or None


def _wanted_format():
    fmt = (os.environ.get(_FORMAT_ENV) or "MP3_320").strip().upper()
    return fmt if fmt in _FORMAT_NUMBERS else "MP3_320"


def _gw_headers(arl):
    return {"User-Agent": "Parlay/0.1",
            "Cookie": "arl=" + arl}


def _get_api_token(arl):
    """Scrape the gw-light api_token from the Deezer homepage.

    The homepage embeds `"api_token":"<token>"` in its bootstrapping
    JSON; the token is per-session and only valid with the arl cookie.
    """
    req = urllib.request.Request("https://www.deezer.com/",
                                 headers=_gw_headers(arl))
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            html = resp.read().decode("utf-8", "replace")
    except Exception as exc:
        raise download_failed("Deezer homepage fetch failed: %s" % exc)
    m = re.search(r'"api_token"\s*:\s*"([A-Za-z0-9]+)"', html)
    if not m:
        raise download_failed("Deezer api_token not found on homepage "
                              "(ARL expired or Deezer changed its page)")
    return m.group(1)


def _gw_call(method, arl, api_token, payload):
    """One gw-light call; returns the `results` dict.

    gw-light answers {"error": {...}, "results": {...}}; a non-empty
    error dict means the call failed (bad token, expired ARL, ...).
    """
    url = ("%s?method=%s&api_version=1.0&api_token=%s&input=3"
           % (_GW_LIGHT, method, api_token))
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url, data=data,
        headers={**_gw_headers(arl), "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            body = json.loads(resp.read().decode("utf-8"))
    except Exception as exc:
        raise download_failed("Deezer gw-light %s failed: %s" % (method, exc))
    if isinstance(body, dict) and body.get("error"):
        raise download_failed("Deezer gw-light %s error: %s"
                              % (method, body["error"]))
    results = (body or {}).get("results")
    if not isinstance(results, dict):
        raise download_failed("Deezer gw-light %s: no results" % method)
    return results


def _song_get_data(track_id, arl, api_token):
    """gw-light song.getData -> track record (MD5_ORIGIN, TRACK_TOKEN,
    MEDIA_VERSION, FILESIZE_*)."""
    return _gw_call("song.getData", arl, api_token, {"sng_id": track_id})


def _license_token(arl, api_token):
    """gw-light deezer.getUserData -> OPTIONS.license_token (None when
    the account has none — the modern get_url path is then skipped and
    the legacy URL is used)."""
    try:
        data = _gw_call("deezer.getUserData", arl, api_token, {})
        return (data.get("OPTIONS") or {}).get("license_token") or None
    except Exception:
        return None


def _modern_track_url(track_token, fmt, license_token):
    """POST media.deezer.com/v1/get_url -> CDN URL for the track."""
    payload = {"license_token": license_token,
               "track_tokens": [track_token],
               "format": fmt}
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        "https://media.deezer.com/v1/get_url", data=data,
        headers={"Content-Type": "application/json",
                 "User-Agent": "Parlay/0.1"})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            body = json.loads(resp.read().decode("utf-8"))
    except Exception as exc:
        raise download_failed("Deezer media get_url failed: %s" % exc)
    try:
        url = body["data"][0]["media"][0]["sources"][0]["url"]
    except (KeyError, IndexError, TypeError):
        raise download_failed("Deezer media get_url: no URL in response")
    if not url:
        raise download_failed("Deezer media get_url: empty URL")
    return url


def _legacy_stream_url(sng_id, md5, media_version, format_num):
    """deemix generateCryptedStreamURL: AES-ECB-encrypted stream path.

    urlPart = md5 \\xa4 format \\xa4 sng_id \\xa4 media_version;
    step2 = md5(urlPart) \\xa4 urlPart \\xa4, '.'-padded to a multiple
    of 16; AES-ECB encrypt with the legacy key, hex-encode, and hang
    it under e-cdns-proxy-<first md5 char>.dzcdn.net/mobile/1/.
    """
    if _AES is None:
        raise download_failed("pycryptodomex is required for the "
                              "Deezer ARL legacy URL (AES-ECB)")
    url_part = b"\xa4".join([md5.encode(), str(format_num).encode(),
                             str(sng_id).encode(),
                             str(media_version).encode()])
    md5val = hashlib.md5(url_part).hexdigest().encode()
    step2 = md5val + b"\xa4" + url_part + b"\xa4"
    step2 += b"." * (16 - (len(step2) % 16))
    cipher = _AES.new(_LEGACY_URL_KEY, _AES.MODE_ECB)
    hexed = cipher.encrypt(step2).hex()
    return "https://e-cdns-proxy-%s.dzcdn.net/mobile/1/%s" % (md5[0], hexed)


def _blowfish_key(track_id):
    """deemix generateBlowfishKey: MD5(str(track_id)) hex, XOR-fold
    chars i ^ i+16 ^ SECRET[i] for i in 0..15 -> 16-byte key."""
    md5hex = hashlib.md5(str(track_id).encode()).hexdigest()
    key = "".join(
        chr(ord(md5hex[i]) ^ ord(md5hex[i + 16]) ^ ord(_BLOWFISH_SECRET[i]))
        for i in range(16))
    return key.encode()


def _decrypt_chunk(key, data):
    """deemix decryptChunk: Blowfish-CBC, fixed IV 00 01 .. 07."""
    if _Blowfish is None:
        raise download_failed("pycryptodomex is required for Deezer "
                              "ARL decryption (Blowfish)")
    return _Blowfish.new(key, _Blowfish.MODE_CBC, _BLOWFISH_IV).decrypt(data)


def _download_full(url, track_id, dest_path):
    """Stream a full track from the CDN, Blowfish-decrypting the
    encrypted chunks (deemix streamTrack: 6144-byte chunks, the first
    2048 bytes of each chunk are decrypted when the URL is a crypted
    /mobile/ or /media/ stream). Returns the file path."""
    crypted = "/mobile/" in url or "/media/" in url
    key = _blowfish_key(track_id) if crypted else None
    req = urllib.request.Request(url, headers={"User-Agent": "Parlay/0.1"})
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            if resp.status != 200:
                raise download_failed(
                    "Deezer full-track download: HTTP %s" % resp.status)
            with open(dest_path, "wb") as fh:
                while True:
                    chunk = resp.read(6144)
                    if not chunk:
                        break
                    if crypted and len(chunk) >= 2048:
                        chunk = _decrypt_chunk(key, chunk[:2048]) + chunk[2048:]
                    fh.write(chunk)
    except download_failed:
        raise
    except Exception as exc:
        raise download_failed("Deezer full-track download failed: %s" % exc)
    if os.path.getsize(dest_path) < 1024:
        raise download_failed("Deezer full-track download produced an empty file")
    return dest_path


def _pick_format(song_data):
    """Pick the best available format: the requested one if its
    FILESIZE_* is non-zero, else MP3_320, else MP3_128. Returns
    (format_name, format_num) or None when the account cannot stream
    the track at all (e.g. geo-locked)."""
    wanted = _wanted_format()
    order = [wanted, "MP3_320", "MP3_128"]
    seen = set()
    for fmt in order:
        if fmt in seen:
            continue
        seen.add(fmt)
        size = song_data.get("FILESIZE_" + fmt)
        if size and str(size) != "0":
            return fmt, _FORMAT_NUMBERS[fmt]
    return None


def _try_arl_full(track_id, dest_dir, name):
    """Try the ARL full-track path. Returns (file_path, format_name)
    or None on ANY failure — the caller falls back to the preview
    path. Never raises: a failed ARL attempt must not kill the
    download that previews could still serve."""
    arl = _arl()
    if not arl:
        return None
    try:
        api_token = _get_api_token(arl)
        song = _song_get_data(track_id, arl, api_token)
        md5 = song.get("MD5_ORIGIN")
        track_token = song.get("TRACK_TOKEN")
        media_version = song.get("MEDIA_VERSION")
        if not md5:
            print("deezer: ARL song.getData for %s has no MD5_ORIGIN "
                  "(track unavailable for this account)" % track_id,
                  file=sys.stderr)
            return None
        picked = _pick_format(song)
        if picked is None:
            print("deezer: ARL track %s has no streamable format "
                  "for this account" % track_id, file=sys.stderr)
            return None
        fmt_name, fmt_num = picked
        url = None
        if track_token:
            license = _license_token(arl, api_token)
            if license:
                try:
                    url = _modern_track_url(track_token, fmt_name, license)
                except Exception as exc:
                    print("deezer: modern get_url failed for %s (%s); "
                          "trying legacy URL" % (track_id, exc),
                          file=sys.stderr)
        if not url:
            url = _legacy_stream_url(track_id, md5, media_version, fmt_num)
        ext = _FORMAT_EXT[fmt_name]
        out_path = os.path.join(dest_dir, sanitize_filename(name) + ext)
        _download_full(url, track_id, out_path)
        return out_path, fmt_name
    except Exception as exc:
        print("deezer: ARL full-track failed for %s: %s" % (track_id, exc),
              file=sys.stderr)
        return None


def download_track_by_id(track_id, dest_dir):
    """Download one Deezer track; returns (file_path, track)."""
    data = get_track(track_id)
    track = _track_from_api(data)
    # Full track first when a Deezer login is configured (ARL path
    # below); yt-dlp refuses Deezer outright, so there is no point
    # trying it before the authenticated path.
    if arl_configured():
        full = _try_arl_full(track_id, dest_dir, "%s - %s" % (
            track.get("artist") or "deezer", track.get("title") or track_id))
        if full:
            path, fmt_name = full
            track["note"] = "full track via Deezer ARL (%s)" % fmt_name
            return path, track
        print("deezer: ARL full-track attempt failed for %s; "
              "falling back to preview" % track_id, file=sys.stderr)
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
                path = None
                if arl_configured():
                    full = _try_arl_full(str(entry.get("id")), job_dir, name)
                    if full:
                        path, fmt_name = full
                        track["note"] = ("full track via Deezer ARL (%s)"
                                         % fmt_name)
                if path is None:
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
        full_count = sum(1 for t, _ in downloaded
                         if "via Deezer ARL" in (t.get("note") or ""))
        if full_count == len(downloaded):
            first["note"] = ("album ZIP of %d full tracks (Deezer ARL)"
                             % len(downloaded))
        elif full_count:
            first["note"] = ("album ZIP of %d tracks (%d full via Deezer "
                             "ARL, rest previews)"
                             % (len(downloaded), full_count))
        else:
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
