"""Tests for the yt-dlp worker's cookieless-first cookie policy.

Probe 2026-11-05: passing cookies switches yt-dlp's default client
set to the logged-in set (web_embedded/tv_samsung/web, web_creator
for premium), whose web/web_creator formats need GVS PO tokens — the
VPS probe got a URL with cookies that 403'd on fetch. So the daemon
is cookieless first and retries with cookies ONLY on age/login gates.

Run with the yt-dlp venv python (system python3 does NOT have
yt_dlp): scripts/yt-dlp-venv/bin/python scripts/test_yt_dlp_worker.py
(no pytest dependency — a minimal runner at the bottom runs each
test_* function).
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import yt_dlp_worker as w


def test_needs_auth_age_gate():
    assert w._needs_auth("ERROR: Age-restricted video")
    assert w._needs_auth("Sign in to confirm your age")
    assert w._needs_auth("This video is only available to Music Premium members")
    assert w._needs_auth("Private video. Sign in if you've been granted access.")
    assert w._needs_auth("Login required to access this video")


def test_needs_auth_bot_check():
    # "Sign in to confirm you're not a bot" is an IP/bot check, not
    # an age gate — but it contains the same "sign in to confirm"
    # phrase, so it retries with cookies (a logged-in session is the
    # only thing that could help there).
    assert w._needs_auth("Sign in to confirm you're not a bot")


def test_needs_auth_not_auth():
    assert not w._needs_auth("ERROR: unable to download video data: HTTP Error 403: Forbidden")
    assert not w._needs_auth("android_vr client https formats require a GVS PO Token which was not provided")
    assert not w._needs_auth("Video unavailable")
    assert not w._needs_auth("Unable to extract video data")
    assert not w._needs_auth("")
    assert not w._needs_auth(None)


def test_warm_instance_is_cookieless_even_with_cookie_file():
    """The warm _ydl() instance must never carry a cookie file, even
    when one exists on disk — that is the whole policy."""
    import tempfile

    with tempfile.TemporaryDirectory() as td:
        cookie = os.path.join(td, "cookies.txt")
        with open(cookie, "w") as f:
            f.write("# Netscape HTTP Cookie File\n")
        old = os.environ.get("YTDLP_COOKIEFILE")
        os.environ["YTDLP_COOKIEFILE"] = cookie
        try:
            w._YDL = None
            w._YDL_COOKIES = None
            ydl = w._ydl()
            assert "cookiefile" not in ydl.params, "warm instance carries a cookie file"
            # The cookies instance DOES carry it (and only it).
            ydl_c = w._ydl_cookies()
            assert ydl_c is not None
            assert ydl_c.params.get("cookiefile") == cookie
        finally:
            if old is None:
                os.environ.pop("YTDLP_COOKIEFILE", None)
            else:
                os.environ["YTDLP_COOKIEFILE"] = old
            w._YDL = None
            w._YDL_COOKIES = None


def test_cookies_instance_is_none_without_cookie_file():
    import tempfile

    saved = {}
    for k in ("YTDLP_COOKIEFILE", "YTDLP_COOKIES", "YOUTUBE_COOKIEFILE",
              "YOUTUBE_COOKIES", "COOKIE_FILE"):
        saved[k] = os.environ.pop(k, None)
    old_home = os.environ.get("HOME")
    try:
        with tempfile.TemporaryDirectory() as td:
            # Point $HOME at the empty tmp dir so ~/cookies.txt is
            # not found, and the repo data/cookies.txt is not under it.
            os.environ["HOME"] = td
            w._YDL_COOKIES = None
            assert w._ydl_cookies() is None
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        if old_home is None:
            os.environ.pop("HOME", None)
        else:
            os.environ["HOME"] = old_home
        w._YDL_COOKIES = None


if __name__ == "__main__":
    # Minimal runner (no pytest dependency): run each test_* function.
    import traceback

    failures = 0
    for name, fn in sorted(list(globals().items())):
        if not name.startswith("test_"):
            continue
        try:
            fn()
            print(f"PASS {name}")
        except Exception:
            failures += 1
            print(f"FAIL {name}")
            traceback.print_exc()
    sys.exit(1 if failures else 0)
