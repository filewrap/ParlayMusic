#!/usr/bin/env python3
"""Persistent yt-dlp worker daemon for the Parlay Go backend.

ONE process started once at Go boot, persistent stdin/stdout
JSON-RPC lines, warm yt_dlp.YoutubeDL in RAM, zero per-call forks
(FINAL-ARCHITECTURE.md §1.3 Part 2, §8.1). This is the ONLY yt-dlp
path in the new architecture: the fallback when native InnerTube
hits an age-gated or signature-ciphered edge case, the TikTok
metadata path, and the YouTube/Spotify download path.

Protocol: one JSON object per line, both directions.
  Request:  {"id": <int>, "method": <str>, "params": {...}}
  Response: {"id": <int>, "result": {...}}
  Error:    {"id": <int>, "error": {"code": <str>, "message": <str>}}

Methods:
  ping     -> {"ok": true} (no network)
  version  -> {"version": "<yt-dlp version>"}
  extract  -> {"url": ...} -> yt-dlp -J metadata (download=False)
  download -> {"url": ..., "dir": ...} -> bestaudio, extracted to mp3,
              {"file_path": ...}

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


def _ydl():
    """One warm YoutubeDL instance for the life of the process."""
    global _YDL
    if _YDL is None:
        _YDL = yt_dlp.YoutubeDL({
            "quiet": True,
            "no_warnings": True,
            "noprogress": True,
            "noplaylist": True,
            "socket_timeout": 30,
            "retries": 3,
        })
    return _YDL


def _extract(params):
    url = (params or {}).get("url", "")
    if not url:
        raise ValueError("extract: url is required")
    return _ydl().extract_info(url, download=False)


def _download(params):
    url = (params or {}).get("url", "")
    outdir = (params or {}).get("dir", "")
    if not url:
        raise ValueError("download: url is required")
    if not outdir:
        raise ValueError("download: dir is required")
    os.makedirs(outdir, exist_ok=True)
    outtmpl = os.path.join(outdir, "%(title).150B [%(id)s].%(ext)s")
    ydl = yt_dlp.YoutubeDL({
        "quiet": True,
        "no_warnings": True,
        "noprogress": True,
        "noplaylist": True,
        "socket_timeout": 30,
        "retries": 3,
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
    before = set(os.listdir(outdir))
    ydl.download([url])
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
