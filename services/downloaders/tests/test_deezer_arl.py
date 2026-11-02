"""Deezer ARL full-track tests (Worker D, Phase 5).

All HTTP is stubbed (no real cookie, no network to Deezer): the
gw-light / media.get_url / CDN endpoints are faked with canned
responses, and the Blowfish/AES steps are verified by encrypt-then-
decrypt round-trips against the real pycryptodomex primitives.
Run offline:
    .venv/bin/python -m unittest discover -s tests -v
"""
import hashlib
import io
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from src import deezer as deezer_mod

TRACK_ID = "3135556"
ARL = "test-arl-value-never-real"


class FakeResp:
    """Minimal urlopen response: context manager + status + read(n)."""

    def __init__(self, body, status=200):
        self._buf = io.BytesIO(body if isinstance(body, bytes) else body.encode())
        self.status = status

    def read(self, n=-1):
        return self._buf.read(n)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def _gw_body(results):
    return json.dumps({"error": {}, "results": results}).encode()


class StubHTTP:
    """Dispatcher for urllib.request.urlopen, keyed on URL substring."""

    def __init__(self, routes):
        # routes: [(substring, body-bytes-or-callable), ...]
        self.routes = routes
        self.seen = []

    def __call__(self, req, timeout=None):
        url = req.full_url if hasattr(req, "full_url") else str(req)
        self.seen.append(url)
        for needle, body in self.routes:
            if needle in url:
                if callable(body):
                    body = body(req)
                return FakeResp(body)
        raise AssertionError("unexpected URL in stub: %s" % url)


def _encrypted_cdn_bytes(track_id, plaintext):
    """Encrypt the first 2048 bytes of each 6144-byte chunk the way
    Deezer's CDN serves them (Blowfish-CBC, track key, fixed IV)."""
    from Cryptodome.Cipher import Blowfish
    key = deezer_mod._blowfish_key(track_id)
    iv = bytes([0, 1, 2, 3, 4, 5, 6, 7])
    out = bytearray()
    for off in range(0, len(plaintext), 6144):
        chunk = plaintext[off:off + 6144]
        if len(chunk) >= 2048:
            cipher = Blowfish.new(key, Blowfish.MODE_CBC, iv)
            out += cipher.encrypt(chunk[:2048]) + chunk[2048:]
        else:
            out += chunk
    return bytes(out)


class TestBlowfishAndLegacyURL(unittest.TestCase):
    def test_blowfish_key_is_16_bytes(self):
        key = deezer_mod._blowfish_key(TRACK_ID)
        self.assertEqual(len(key), 16)
        # Independent recomputation of the deemix formula (same inputs,
        # written out longhand here so a typo in the module's SECRET
        # constant would show up as a mismatch).
        md5hex = hashlib.md5(TRACK_ID.encode()).hexdigest()
        secret = "g4el58wc0zvf9na1"
        expect = "".join(
            chr(ord(md5hex[i]) ^ ord(md5hex[i + 16]) ^ ord(secret[i]))
            for i in range(16)).encode()
        self.assertEqual(key, expect)

    def test_decrypt_chunk_roundtrip(self):
        from Cryptodome.Cipher import Blowfish
        key = deezer_mod._blowfish_key(TRACK_ID)
        iv = bytes([0, 1, 2, 3, 4, 5, 6, 7])
        plain = b"A" * 2048  # one full Blowfish-CBC block run
        cipher = Blowfish.new(key, Blowfish.MODE_CBC, iv)
        encrypted = cipher.encrypt(plain)
        self.assertEqual(deezer_mod._decrypt_chunk(key, encrypted), plain)

    def test_legacy_stream_url_reverses(self):
        from Cryptodome.Cipher import AES
        md5 = "abc123def4567890abc123def4567890"
        url = deezer_mod._legacy_stream_url(TRACK_ID, md5, "1", 3)
        self.assertTrue(url.startswith(
            "https://e-cdns-proxy-a.dzcdn.net/mobile/1/"))
        hexed = url.rsplit("/", 1)[-1]
        cipher = AES.new(b"jo6aey6haid2Teih", AES.MODE_ECB)
        step2 = cipher.decrypt(bytes.fromhex(hexed))
        parts = step2.split(b"\xa4")
        # [md5(urlPart), md5, format, sng_id, media_version, padding]
        self.assertEqual(parts[1], md5.encode())
        self.assertEqual(parts[2], b"3")
        self.assertEqual(parts[3], TRACK_ID.encode())
        self.assertEqual(parts[4], b"1")

    def test_pick_format_ladder(self):
        with mock.patch.dict(os.environ, {"DEEZER_FORMAT": "FLAC"}):
            # FLAC unavailable -> MP3_320.
            self.assertEqual(
                deezer_mod._pick_format({"FILESIZE_FLAC": "0",
                                         "FILESIZE_MP3_320": "123",
                                         "FILESIZE_MP3_128": "45"}),
                ("MP3_320", 3))
            # Nothing streamable -> None.
            self.assertIsNone(deezer_mod._pick_format(
                {"FILESIZE_FLAC": "0", "FILESIZE_MP3_320": "0",
                 "FILESIZE_MP3_128": "0"}))
            # Requested format available -> it wins.
            self.assertEqual(
                deezer_mod._pick_format({"FILESIZE_FLAC": "999",
                                         "FILESIZE_MP3_320": "123"}),
                ("FLAC", 9))
        with mock.patch.dict(os.environ, {"DEEZER_FORMAT": "bogus"}):
            self.assertEqual(deezer_mod._wanted_format(), "MP3_320")


class TestARLFlow(unittest.TestCase):
    SONG_DATA = {"MD5_ORIGIN": "abc123def4567890abc123def4567890",
                 "TRACK_TOKEN": "track-token-1",
                 "MEDIA_VERSION": "1",
                 "FILESIZE_MP3_320": "4000000",
                 "FILESIZE_MP3_128": "1500000",
                 "FILESIZE_FLAC": "0",
                 "SNG_ID": TRACK_ID}

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="parlay-deezer-arl-")
        self.addCleanup(lambda: __import__("shutil").rmtree(
            self.tmp, ignore_errors=True))
        self.env = mock.patch.dict(
            os.environ, {"DEEZER_ARL": ARL, "DEEZER_FORMAT": "MP3_320"})
        self.env.start()
        self.addCleanup(self.env.stop)

    def _routes(self, cdn_body, license_token="lic-1"):
        # NOTE: order matters — "www.deezer.com/" also matches the
        # gw-light URL, so the specific routes come first.
        return [
            ("method=song.getData", _gw_body(self.SONG_DATA)),
            ("method=deezer.getUserData",
             _gw_body({"OPTIONS": {"license_token": license_token}})),
            ("media.deezer.com/v1/get_url",
             json.dumps({"data": [{"media": [{"sources": [
                 {"url": "https://e-cdns-proxy-a.dzcdn.net/media/1/xyz"}]}]}]}).encode()),
            ("dzcdn.net", cdn_body),
            ("www.deezer.com/",
             b'<html>{"api_token":"testtoken123"}</html>'),
        ]

    def test_modern_url_flow_decrypts(self):
        plaintext = (b"ID3" + bytes(range(256)) * 40)[:7000]
        cdn = _encrypted_cdn_bytes(TRACK_ID, plaintext)
        stub = StubHTTP(self._routes(cdn))
        with mock.patch.object(deezer_mod.urllib.request, "urlopen", stub):
            got = deezer_mod._try_arl_full(TRACK_ID, self.tmp, "Artist - Title")
        self.assertIsNotNone(got, "ARL flow should succeed against stubs")
        path, fmt = got
        self.assertEqual(fmt, "MP3_320")
        self.assertTrue(path.endswith(".mp3"))
        with open(path, "rb") as fh:
            self.assertEqual(fh.read(), plaintext)
        self.assertTrue(any("method=song.getData" in u for u in stub.seen))
        self.assertTrue(any("media.deezer.com" in u for u in stub.seen))

    def test_legacy_fallback_when_no_license_token(self):
        plaintext = (b"FLAC" + bytes(range(256)) * 40)[:7000]
        cdn = _encrypted_cdn_bytes(TRACK_ID, plaintext)
        stub = StubHTTP(self._routes(cdn, license_token=None))
        with mock.patch.object(deezer_mod.urllib.request, "urlopen", stub):
            got = deezer_mod._try_arl_full(TRACK_ID, self.tmp, "Artist - Title")
        self.assertIsNotNone(got)
        path, fmt = got
        self.assertEqual(fmt, "MP3_320")
        with open(path, "rb") as fh:
            self.assertEqual(fh.read(), plaintext)
        self.assertTrue(any("/mobile/1/" in u for u in stub.seen),
                        "legacy e-cdns-proxy URL should have been used")

    def test_arl_failure_returns_none(self):
        def boom(req, timeout=None):
            raise Exception("network down")
        with mock.patch.object(deezer_mod.urllib.request, "urlopen", boom):
            self.assertIsNone(
                deezer_mod._try_arl_full(TRACK_ID, self.tmp, "Artist - Title"))

    def test_no_arl_means_no_attempt(self):
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("DEEZER_ARL", None)
            self.assertFalse(deezer_mod.arl_configured())
            self.assertIsNone(
                deezer_mod._try_arl_full(TRACK_ID, self.tmp, "x"))


if __name__ == "__main__":
    unittest.main()
