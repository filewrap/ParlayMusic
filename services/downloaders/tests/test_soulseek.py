"""Soulseek (slskd) tests (Worker D, Phase 5).

A stub slskd HTTP server (real http.server on loopback) implements
the slskd API surface the client uses, so search / score / match /
download are all exercised without a Soulseek account or network.
Run offline:
    .venv/bin/python -m unittest discover -s tests -v
"""
import json
import os
import shutil
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from src.soulseek import client as slskd_client
from src.soulseek import downloader as slskd_dl
from src.soulseek import matcher as slskd_matcher
from src.soulseek import scorer as slskd_scorer
from src.soulseek import selection as slskd_selection
from src.soulseek.client import SearchResult

# Canned peer responses for "Daft Punk One More Time".
RESPONSES = [
    {"username": "fastpeer", "hasFreeUploadSlot": True, "uploadSpeed": 5000000,
     "queueLength": 0, "files": [
        {"filename": "Music\\Daft Punk - Discovery\\01 One More Time.flac",
         "size": 30000000, "bitDepth": 16, "sampleRate": 44100, "length": 434},
        {"filename": "Music\\Daft Punk - Discovery\\01 One More Time.mp3",
         "size": 7000000, "bitRate": 320, "length": 434},
    ]},
    {"username": "slowpeer", "hasFreeUploadSlot": False, "uploadSpeed": 100000,
     "queueLength": 12, "files": [
        {"filename": "Music\\Daft Punk - Discovery\\01 One More Time.flac",
         "size": 30000000, "bitDepth": 16, "sampleRate": 44100, "length": 434},
        {"filename": "Music\\Daft Punk - Discovery\\01 One More Time (live).flac",
         "size": 31000000, "bitDepth": 16, "sampleRate": 44100, "length": 440},
    ]},
    {"username": "wrongartist", "hasFreeUploadSlot": True, "uploadSpeed": 9000000,
     "queueLength": 0, "files": [
        {"filename": "Music\\Bird Sounds\\Barnacle.flac",
         "size": 20000000, "bitDepth": 16, "sampleRate": 44100, "length": 434},
    ]},
]


class StubSlskd:
    """In-memory slskd: searches always complete instantly with RESPONSES;
    enqueued files immediately report Completed, Succeeded."""

    def __init__(self):
        self.enqueued = []  # (username, [files])
        self.fail_searches_with = None  # (status, body) to fail POST /searches

    def handler_class(self):
        stub = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def _send(self, status, obj):
                body = json.dumps(obj).encode() if obj is not None else b""
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def _read_json(self):
                length = int(self.headers.get("Content-Length") or 0)
                raw = self.rfile.read(length) if length else b""
                return json.loads(raw.decode()) if raw else {}

            def do_GET(self):
                if self.path == "/api/v0/application":
                    return self._send(200, {"version": {"current": "0.26.2.0"}})
                if self.path.startswith("/api/v0/searches/") and self.path.endswith("/responses"):
                    return self._send(200, RESPONSES)
                if self.path.startswith("/api/v0/searches/"):
                    return self._send(200, {"id": "search-1", "isComplete": True,
                                            "fileCount": 5, "responseCount": 3})
                if self.path == "/api/v0/transfers/downloads/":
                    rows = []
                    for username, files in stub.enqueued:
                        rows.append({"username": username, "directories": [{
                            "directory": "Music",
                            "files": [{
                                "id": "tx-%s-%s" % (username, i),
                                "filename": f["filename"],
                                "size": f["size"],
                                "state": "Completed, Succeeded",
                                "bytesTransferred": f["size"],
                                "averageSpeed": 1000000,
                                "percentComplete": 100,
                            } for i, f in enumerate(files)]}]})
                    return self._send(200, rows)
                if self.path.startswith("/api/v0/transfers/downloads/"):
                    # Single-user view: {"username", "directories": [...]}.
                    username = self.path.rsplit("/", 1)[-1]
                    files = []
                    for u, fs in stub.enqueued:
                        if u == username:
                            files.extend(fs)
                    return self._send(200, {"username": username, "directories": [{
                        "directory": "Music",
                        "files": [{
                            "id": "tx-%s-%s" % (username, i),
                            "filename": f["filename"],
                            "size": f["size"],
                            "state": "Completed, Succeeded",
                            "bytesTransferred": f["size"],
                            "averageSpeed": 1000000,
                            "percentComplete": 100,
                        } for i, f in enumerate(files)]}]})
                return self._send(404, {"error": "nope"})

            def do_POST(self):
                if self.path == "/api/v0/searches":
                    if stub.fail_searches_with:
                        status, body = stub.fail_searches_with
                        return self._send(status, {"error": body})
                    self._read_json()
                    return self._send(201, {"id": "search-1", "isComplete": False,
                                            "fileCount": 0})
                if self.path.startswith("/api/v0/transfers/downloads/"):
                    username = self.path.rsplit("/", 1)[-1]
                    files = self._read_json()
                    stub.enqueued.append((username, files))
                    return self._send(201, None)
                return self._send(404, {"error": "nope"})

            def do_PUT(self):
                if self.path.startswith("/api/v0/searches/"):
                    return self._send(200, None)
                return self._send(404, {"error": "nope"})

            def do_DELETE(self):
                return self._send(200, None)

        return H

    def start(self):
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), self.handler_class())
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        return "http://127.0.0.1:%d" % self.port

    def stop(self):
        self.server.shutdown()
        self.server.server_close()


class SoulseekTestBase(unittest.TestCase):
    def setUp(self):
        self.stub = StubSlskd()
        self.base = self.stub.start()
        self.addCleanup(self.stub.stop)
        self.tmp = tempfile.mkdtemp(prefix="parlay-soulseek-")
        self.addCleanup(lambda: shutil.rmtree(self.tmp, ignore_errors=True))
        # Point the client at the stub and disable pacing for tests.
        self.env = mock.patch.dict(os.environ, {
            "SLSKD_URL": self.base,
            "SLSKD_API_KEY": "test-key",
            "SLSKD_DOWNLOAD_DIR": os.path.join(self.tmp, ".slskd-downloads"),
        })
        self.env.start()
        self.addCleanup(self.env.stop)
        slskd_client.SEARCH_MIN_INTERVAL_SECS = 0
        slskd_client._ZERO_BURST_COOLDOWN_SECS = 0
        slskd_client._next_search_start = 0
        slskd_client._zero_streak = 0
        slskd_client._zero_verified_until = 0

    def _stage_file(self, username, relpath, content=b"FLACBYTES"):
        """Place a file where slskd would have downloaded it."""
        path = os.path.join(self.tmp, ".slskd-downloads", username, relpath)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(content)
        return path


class TestClient(SoulseekTestBase):
    def test_search_returns_peer_responses(self):
        responses = slskd_client.search("Daft Punk One More Time")
        self.assertEqual(len(responses), 3)
        self.assertEqual({r["username"] for r in responses},
                         {"fastpeer", "slowpeer", "wrongartist"})

    def test_parse_files_lossless_only(self):
        files = slskd_client.parse_files(RESPONSES, lossless_only=True)
        self.assertEqual(len(files), 4)  # 3 flac + 1 live flac; mp3 excluded
        self.assertTrue(all(f.extension == "flac" for f in files))
        all_files = slskd_client.parse_files(RESPONSES, lossless_only=False)
        self.assertEqual(len(all_files), 5)  # + mp3

    def test_search_error_when_slskd_down(self):
        with mock.patch.dict(os.environ, {"SLSKD_URL": "http://127.0.0.1:1"}):
            with self.assertRaises(slskd_client.SearchError):
                slskd_client.search("anything")

    def test_search_not_connected_message(self):
        self.stub.fail_searches_with = (
            400, "The server connection must be connected and logged in "
                 "to perform a search (currently: Disconnected)")
        with self.assertRaises(slskd_client.SearchError) as ctx:
            slskd_client.search("Daft Punk One More Time")
        self.assertIn("Disconnected", str(ctx.exception))

    def test_find_local_file(self):
        staged = self._stage_file(
            "fastpeer", "Music/Daft Punk - Discovery/01 One More Time.flac")
        found = slskd_client.find_local_file(
            os.path.join(self.tmp, ".slskd-downloads"), "fastpeer",
            "Music\\Daft Punk - Discovery\\01 One More Time.flac")
        self.assertEqual(found, staged)
        # Path traversal in a peer username cannot escape the staging dir.
        safe = slskd_client._safe_username("../../etc")
        self.assertNotIn("/", safe)
        self.assertNotIn("..", safe)


class TestScorer(SoulseekTestBase):
    def test_prefers_free_slot_exact_duration(self):
        files = slskd_client.parse_files(RESPONSES, lossless_only=True)
        scored = slskd_scorer.score_track_results(
            files, "Daft Punk", "One More Time", 434)
        # Wrong-artist file excluded by the artist gate.
        self.assertTrue(all(s.result.username != "wrongartist" for s in scored))
        # fastpeer first (free slot, no queue), slowpeer second.
        self.assertEqual(scored[0].result.username, "fastpeer")
        self.assertEqual(scored[0].match_score, 55.0)  # exact duration + full name
        # Live version penalized on the match axis (×0.30).
        live = [s for s in scored if "live" in s.result.basename.lower()]
        self.assertEqual(len(live), 1)
        self.assertLess(live[0].match_score, 20.0)

    def test_excludes_far_duration(self):
        files = [SearchResult("u", "Music\\A\\B.flac", 100, length=100)]
        scored = slskd_scorer.score_track_results(files, "A", "B", 434)
        self.assertEqual(scored, [])

    def test_folder_scoring_complete_beats_partial(self):
        files = slskd_client.parse_files(RESPONSES, lossless_only=True)
        folders = slskd_client.group_by_folder(files)
        scored = slskd_scorer.score_folder_results(
            folders, [434], ["One More Time"], "Daft Punk", "Discovery")
        self.assertTrue(scored)
        best = scored[0]
        self.assertEqual(best.missing_count, 0)
        self.assertEqual(best.folder.username, "fastpeer")


class TestMatcherSelection(SoulseekTestBase):
    def test_query_building_diacritics(self):
        primary, fallbacks = slskd_matcher._build_track_queries("Beyoncé", "Halo")
        self.assertIn("Beyoncé", primary)
        self.assertIn("Beyonce Halo", fallbacks)

    def test_find_track_confident_pick(self):
        auto, alts = slskd_matcher.find_track(
            {"title": "One More Time", "artist": "Daft Punk",
             "duration_sec": 434})
        self.assertIsNotNone(auto)
        self.assertEqual(auto.result.username, "fastpeer")
        self.assertEqual(auto.sources[0].username, "fastpeer")
        # Same recording on slowpeer is kept as a retry source.
        self.assertEqual(len(auto.sources), 2)

    def test_search_satisfied_thresholds(self):
        good = slskd_scorer.ScoredTrack(None, 80.0, match_score=47.0)
        self.assertTrue(slskd_selection.search_satisfied(good))
        weak = slskd_scorer.ScoredTrack(None, 30.0, match_score=19.0)
        self.assertFalse(slskd_selection.search_satisfied(weak))
        self.assertLess(weak.match_score, slskd_selection.MATCH_FLOOR)

    def test_group_copies_keeps_retry_sources(self):
        a = SearchResult("p1", "Music\\X\\01 Song.flac", 10, length=200)
        b = SearchResult("p2", "Music\\X\\01 Song.flac", 10, length=200)
        scored = [slskd_scorer.ScoredTrack(a, 50.0, match_score=50.0),
                  slskd_scorer.ScoredTrack(b, 49.0, match_score=49.0)]
        grouped = slskd_selection.group_copies(scored)
        self.assertEqual(len(grouped), 1)
        self.assertEqual(len(grouped[0].sources), 2)


class TestDownloader(SoulseekTestBase):
    def test_download_single_track_e2e(self):
        self._stage_file(
            "fastpeer", "Music/Daft Punk - Discovery/01 One More Time.flac",
            b"FLACBYTES" * 100)
        dest = os.path.join(self.tmp, "out")
        path, downloaded, fmt, peer = slskd_dl.download_single_track(
            {"title": "One More Time", "artist": "Daft Punk",
             "duration_sec": 434}, dest)
        self.assertTrue(downloaded)
        self.assertEqual(peer, "fastpeer")
        self.assertEqual(fmt, "FLAC 16-bit 44kHz")
        self.assertTrue(os.path.isfile(path))
        with open(path, "rb") as fh:
            self.assertEqual(fh.read(), b"FLACBYTES" * 100)
        # slskd was actually asked to enqueue from fastpeer.
        self.assertEqual(self.stub.enqueued[0][0], "fastpeer")

    def test_download_single_track_no_match(self):
        with self.assertRaises(RuntimeError) as ctx:
            slskd_dl.download_single_track(
                {"title": "Nonexistent Song Zzz", "artist": "Nobody",
                 "duration_sec": 200}, os.path.join(self.tmp, "out"))
        # The stub returns the same responses for every query, so the
        # artist gate excludes everything: honest "none matched".
        self.assertIn("files found but none matched", str(ctx.exception))

    def test_provider_download(self):
        from src import providers as providers_mod
        self._stage_file(
            "fastpeer", "Music/Daft Punk - Discovery/01 One More Time.flac",
            b"FLACBYTES" * 10)
        body = {"query": "Daft Punk - One More Time", "duration_sec": 434,
                "_dl_dir": os.path.join(self.tmp, "out")}
        data = providers_mod.soulseek_download(body)
        self.assertEqual(data["peer"], "fastpeer")
        self.assertTrue(os.path.isfile(data["file_path"]))
        self.assertFalse(data["oversize"])

    def test_provider_slskd_down_is_engine_unavailable(self):
        from src import providers as providers_mod
        from src.util import ApiError
        with mock.patch.dict(os.environ, {"SLSKD_URL": "http://127.0.0.1:1"}):
            with self.assertRaises(ApiError) as ctx:
                providers_mod.soulseek_download(
                    {"query": "x", "_dl_dir": self.tmp})
            self.assertEqual(ctx.exception.code, "engine_unavailable")


if __name__ == "__main__":
    unittest.main()
