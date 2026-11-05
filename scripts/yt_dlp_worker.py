#!/usr/bin/env python3
"""Persistent yt-dlp worker daemon for the Parlay Go backend.

ONE process started once at Go boot, persistent stdin/stdout
JSON-RPC lines, warm yt_dlp.YoutubeDL in RAM, zero per-call forks
(FINAL-ARCHITECTURE.md §1.3 Part 2, §8.1). This is the ONLY yt-dlp
path in the new architecture: the fallback when native InnerTube
hits an age-gated or signature-ciphered edge case, the TikTok
metadata path, and the YouTube/Spotify download path.

Cookie policy (probe 2026-11-05): COOKIELESS FIRST. Passing cookies
switches yt-dlp's default client set to the logged-in set
(web_embedded/tv_samsung/web, web_creator for premium), whose
web/web_creator formats need GVS PO tokens — without a PO provider
those formats are skipped or 403 (the VPS probe got a URL with
cookies that 403'd on fetch). So the warm instance never carries a
cookie file; cookies are used ONLY as a one-shot retry when the
cookieless attempt fails with an age-gate / login error
(_needs_auth), for genuinely age-restricted or members-only videos.

Protocol: one JSON object per line, both directions.
  Request:  {"id": <int>, "method": <str>, "params": {...}}
  Response: {"id": <int>, "result": {...}}
  Error:    {"id": <int>, "error": {"code": <str>, "message": <str>}}

Methods:
  ping     -> {"ok": true} (no network)
  version  -> {"version": "<yt-dlp version>"}
  extract  -> {"url": ..., "po_token": ...} -> yt-dlp -J metadata (download=False)
  download -> {"url": ..., "dir": ..., "po_token": ...} -> bestaudio, extracted to mp3,
              {"file_path": ...}

po_token (optional, both methods) is a yt-dlp PO token in --po-token
form, "web.player+<token>", minted by the bgutil POT sidecar and
handed over by the Go backend (internal/pot). It is passed to
yt-dlp as the Python-API equivalent of
--extractor-args "youtube:po_token=web.player+<token>":
{"extractor_args": {"youtube": {"po_token": ["web.player+<token>"]}}}
— the shape YoutubeIE._get_config_po_token reads via
_configuration_arg('po_token', ..., ie_key=YoutubeIE) in
yt_dlp/extractor/youtube/_video.py. The token is attached ONLY to
the cookieless attempt: an anonymously minted token mixed with
authenticated session cookies can fail validation server-side, so
the cookie retry (genuinely age-gated / members-only videos) runs
tokenless, exactly as before.

Stdout carries ONLY JSON-RPC lines: both YoutubeDL instances run
with "noprogress": True, because yt-dlp's [download] progress bar
is written to stdout with "\\r" separators and would otherwise be
glued onto the same physical line as the JSON response, breaking
the Go client's framing (proven 2026-11-03: the download response
was lost and the caller hung until its timeout).

Run with the yt-dlp venv python (system python3 does NOT have
yt_dlp): scripts/yt-dlp-venv/bin/python scripts/yt_dlp_worker.py
The Go client locates that python itself; this script only requires
that `import yt_dlp` works in whatever interpreter runs it.
"""

import json
import os
import sys

try:
    import yt_dlp
except ImportError as exc:  # pragma: no cover - startup failure
    sys.stderr.write("yt_dlp_worker: yt_dlp is not importable: %s\n" % exc)
    sys.stderr.write("yt_dlp_worker: run with scripts/yt-dlp-venv/bin/python\n")
    raise SystemExit(1)

_YDL = None
_YDL_COOKIES = None


def _find_cookie_file():
    """Locate YouTube/yt-dlp cookies file if available."""
    for env_k in ("YTDLP_COOKIEFILE", "YTDLP_COOKIES", "YOUTUBE_COOKIEFILE", "YOUTUBE_COOKIES", "COOKIE_FILE"):
        val = os.environ.get(env_k)
        if val and os.path.isfile(val):
            return val
    candidates = [
        "/root/cookies.txt",
        os.path.expanduser("~/cookies.txt"),
        os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data", "cookies.txt"),
        os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "cookies.txt"),
    ]
    for c in candidates:
        if os.path.isfile(c):
            return os.path.abspath(c)
    return None


def _base_opts():
    """Opts shared by every YoutubeDL instance built here."""
    return {
        "quiet": True,
        "no_warnings": True,
        "noprogress": True,
        "noplaylist": True,
        "socket_timeout": 30,
        "retries": 3,
        "remote_components": ["ejs:github"],
    }


def _ydl_for_po_token(po_token):
    """A fresh YoutubeDL instance carrying one PO token.

    Built per call, not warm: the warm instance is shared and its
    opts are fixed at construction, while a token is bound to one
    video id. The token is the --po-token CLI value
    ("web.player+<token>"); YoutubeIE._get_config_po_token reads it
    from extractor_args.youtube.po_token (a list) and presents it on
    the web client's player request. The token VALUE is never
    logged.
    """
    opts = _base_opts()
    opts["extractor_args"] = {"youtube": {"po_token": [po_token]}}
    sys.stderr.write("yt-dlp: extract with po_token (client web, context player)\n")
    sys.stderr.flush()
    return yt_dlp.YoutubeDL(opts)


def _ydl():
    """One warm YoutubeDL instance for the life of the process.

    COOKIELESS by design (see module docstring): the default client
    set stays the cookieless one, so no PO-gated formats are pulled
    in. Cookies are only used by _ydl_cookies() as a retry.
    """
    global _YDL
    if _YDL is None:
        _YDL = yt_dlp.YoutubeDL(_base_opts())
    return _YDL


# Substrings (lowercased) in a yt-dlp error that mean "this video
# needs a logged-in session" — age gates, login gates, members-only.
# Anything else (bot-check 403, PO-token 403, unavailable, network)
# is NOT retried with cookies: cookies cannot fix those, and the VPS
# probe proved a cookie retry can hand back a URL that 403s on fetch.
_AUTH_MARKERS = (
    "age-restricted",
    "age restricted",
    "age-verification",
    "age verification",
    "sign in to confirm",
    "login required",
    "log in to",
    "login to",
    "members-only",
    "members only",
    "private video",
    "only available to",
    "premium members",
)


def _needs_auth(msg):
    """True when a yt-dlp failure looks like an age/login gate."""
    m = (msg or "").lower()
    return any(marker in m for marker in _AUTH_MARKERS)


def _ydl_cookies():
    """Warm YoutubeDL instance WITH the cookie file, or None.

    Built lazily on first auth-gated retry only. Returns None when
    no cookie file exists, so the caller can report honestly instead
    of retrying into the void.
    """
    global _YDL_COOKIES
    if _YDL_COOKIES is None:
        cf = _find_cookie_file()
        if not cf:
            return None
        opts = _base_opts()
        opts["cookiefile"] = cf
        _YDL_COOKIES = yt_dlp.YoutubeDL(opts)
    return _YDL_COOKIES


def _extract(params):
    url = (params or {}).get("url", "")
    if not url:
        raise ValueError("extract: url is required")
    po_token = (params or {}).get("po_token", "")
    # The token rides only on the cookieless attempt (see module
    # docstring): the cookie retry below runs tokenless.
    ydl = _ydl_for_po_token(po_token) if po_token else _ydl()
    try:
        return ydl.extract_info(url, download=False)
    except Exception as exc:  # noqa: BLE001 - retry path below
        if not _needs_auth(str(exc)):
            raise
        ydl_c = _ydl_cookies()
        if ydl_c is None:
            raise
        sys.stderr.write(
            "yt-dlp: cookieless extract failed (%s), retrying with cookies\n"
            % str(exc)[:160])
        sys.stderr.flush()
        try:
            return ydl_c.extract_info(url, download=False)
        except Exception as exc2:  # noqa: BLE001 - report both attempts
            raise RuntimeError(
                "extract failed cookieless (%s) and with cookies (%s)"
                % (exc, exc2))


def _download(params):
    url = (params or {}).get("url", "")
    outdir = (params or {}).get("dir", "")
    if not url:
        raise ValueError("download: url is required")
    if not outdir:
        raise ValueError("download: dir is required")
    po_token = (params or {}).get("po_token", "")
    os.makedirs(outdir, exist_ok=True)
    outtmpl = os.path.join(outdir, "%(title).150B [%(id)s].%(ext)s")
    opts = _base_opts()
    opts.update({
        "format": "bestaudio/best",
        "outtmpl": outtmpl,
        "restrictfilenames": True,
        # NOTE: the mp3 extraction is NOT wired by "extractaudio" /
        # "audioformat" params — those keys are only read by the yt-dlp
        # CLI entrypoint (yt_dlp/__init__.py), which translates
        # --extract-audio into a postprocessors list. The Python API
        # needs the postprocessor entry explicitly (proven 2026-11-03:
        # without it the daemon downloaded a raw .webm and skipped the
        # ffmpeg mp3 step entirely, even with the extract keys set).
        "postprocessors": [{
            "key": "FFmpegExtractAudio",
            "preferredcodec": "mp3",
            "preferredquality": "0",
        }],
    })
    # The token rides only on the cookieless attempt (see module
    # docstring); the cookie retry below drops it again.
    if po_token:
        opts["extractor_args"] = {"youtube": {"po_token": [po_token]}}
        sys.stderr.write("yt-dlp: download with po_token (client web, context player)\n")
        sys.stderr.flush()
    # Cookieless first (see module docstring); retry with cookies
    # only on an age/login gate.
    before = set(os.listdir(outdir))
    try:
        yt_dlp.YoutubeDL(opts).download([url])
    except Exception as exc:  # noqa: BLE001 - retry path below
        if not _needs_auth(str(exc)):
            raise
        cf = _find_cookie_file()
        if not cf:
            raise
        sys.stderr.write(
            "yt-dlp: cookieless download failed (%s), retrying with cookies\n"
            % str(exc)[:160])
        sys.stderr.flush()
        opts["cookiefile"] = cf
        # Anonymously minted token + authenticated cookies can fail
        # validation server-side: the cookie retry runs tokenless.
        opts.pop("extractor_args", None)
        try:
            yt_dlp.YoutubeDL(opts).download([url])
        except Exception as exc2:  # noqa: BLE001 - report both attempts
            raise RuntimeError(
                "download failed cookieless (%s) and with cookies (%s)"
                % (exc, exc2))
    after = sorted(
        (os.path.join(outdir, f) for f in os.listdir(outdir)
         if f not in before and os.path.isfile(os.path.join(outdir, f))),
        key=os.path.getmtime,
    )
    if not after:
        raise RuntimeError("yt-dlp download produced no file for %s" % url)
    return {"file_path": after[-1]}


_METHODS = {
    "ping": lambda params: {"ok": True},
    "version": lambda params: {"version": yt_dlp.version.__version__},
    "extract": _extract,
    "download": _download,
}


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except ValueError as exc:
            sys.stdout.write(json.dumps(
                {"id": None, "error": {"code": "bad_request",
                                       "message": "unparsable JSON: %s" % exc}}) + "\n")
            sys.stdout.flush()
            continue
        rid = req.get("id")
        method = req.get("method")
        params = req.get("params") or {}
        try:
            fn = _METHODS.get(method)
            if fn is None:
                raise ValueError("unknown method: %r" % (method,))
            result = fn(params)
            sys.stdout.write(json.dumps({"id": rid, "result": result}) + "\n")
        except Exception as exc:  # noqa: BLE001 - daemon must never die on a bad call
            sys.stdout.write(json.dumps(
                {"id": rid, "error": {"code": "failed",
                                      "message": str(exc)}}) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
