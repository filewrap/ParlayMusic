"""Phase 9 tests: IPC-3 enriched Track fields, IPC-4 download
progress (registry, yt-dlp line parsing, humanised failures, Soulseek
checklist folding, HTTP route + run_id in responses).

Live-network-free: the yt-dlp download and the slskd transfer are
both stubbed/mocked; only pure mapping and the local HTTP server run.
"""
import json
import os
import sys
import tempfile
import threading
import unittest
import urllib.request
import urllib.error
from http.server import ThreadingHTTPServer
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from src import progress as progress_mod
from src import providers as providers_mod
from src import server as server_mod
from src import ytdlp as ytdlp_mod
from src.soulseek import client as slskd_client
from src.soulseek import downloader as slskd_dl
from src.soulseek.client import SearchResult
from src.util import format_duration_str, format_upload_date, track_dict


class TestFormatDurationStr(unittest.TestCase):
    def test_cases(self):
        # Same rendering as vc-go / vc-ts / vc-rust (asserted
        # identically in each service's tests).
        self.assertEqual(format_duration_str(0), "∞")
        self.assertEqual(format_duration_str(-5), "∞")
        self.assertEqual(format_duration_str(None), "∞")
        self.assertEqual(format_duration_str(45), "0:45")
        self.assertEqual(format_duration_str(268), "4:28")
        self.assertEqual(format_duration_str(3599), "59:59")
        self.assertEqual(format_duration_str(3600), "1h")
        self.assertEqual(format_duration_str(3661), "1h 1m 1s")
        self.assertEqual(format_duration_str(3720), "1h 2m")
        self.assertEqual(format_duration_str(3605), "1h 5s")
        self.assertEqual(format_duration_str(213, True), "∞")

    def test_upload_date(self):
        self.assertEqual(format_upload_date("20200115"), "2020-01-15")
        for raw in ("", "2020", "2020011", "2020-01-15", "abcdefgh",
                    "2020AB15"):
            self.assertEqual(format_upload_date(raw), raw)


class TestTrackDictIPC3(unittest.TestCase):
    def test_duration_str_always_derived(self):
        t = track_dict("x", "T", "youtube", duration_sec=268)
        self.assertEqual(t["duration_str"], "4:28")
        self.assertNotIn("views", t)
        self.assertNotIn("uploader", t)
        self.assertNotIn("upload_time", t)
        self.assertNotIn("is_live", t)

    def test_unknown_duration_is_infinity(self):
        t = track_dict("x", "T", "youtube")
        self.assertEqual(t["duration_str"], "∞")

    def test_enriched_fields(self):
        t = track_dict("dQw4w9WgXcQ", "Never Gonna Give You Up", "youtube",
                       artist="Rick Astley", duration_sec=213,
                       views=1600000000, uploader="Rick Astley",
                       upload_time="20091025", is_live=False)
        self.assertEqual(t["duration_str"], "3:33")
        self.assertEqual(t["views"], 1600000000)
        self.assertEqual(t["uploader"], "Rick Astley")
        self.assertEqual(t["upload_time"], "2009-10-25")

    def test_live_stream(self):
        t = track_dict("live1", "Live", "youtube", duration_sec=0,
                       is_live=True)
        self.assertTrue(t["is_live"])
        self.assertEqual(t["duration_str"], "∞")

    def test_zero_views_dropped(self):
        # A "0 views" count is not a view count (prevents a fake row).
        t = track_dict("x", "T", "youtube", duration_sec=90, views=0)
        self.assertNotIn("views", t)


class TestYtdlpEntryIPC3(unittest.TestCase):
    def test_entry_to_track(self):
        entry = {
            "id": "dQw4w9WgXcQ", "title": "Never Gonna Give You Up",
            "uploader": "Rick Astley", "duration": 213,
            "view_count": 1600000000, "upload_date": "20091025",
            "live_status": "not_live",
            "webpage_url": "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
            "thumbnails": [{"url": "https://i.ytimg.com/vi/x/small.jpg"},
                           {"url": "https://i.ytimg.com/vi/x/big.jpg"}],
        }
        t = ytdlp_mod._entry_to_track(entry)
        self.assertEqual(t["duration_str"], "3:33")
        self.assertEqual(t["views"], 1600000000)
        self.assertEqual(t["uploader"], "Rick Astley")
        self.assertEqual(t["upload_time"], "2009-10-25")
        self.assertNotIn("is_live", t)
        self.assertEqual(t["thumbnail"],
                         "https://i.ytimg.com/vi/x/big.jpg")

    def test_live_entry(self):
        entry = {"id": "live1", "title": "Live", "is_live": True,
                 "duration": 0}
        t = ytdlp_mod._entry_to_track(entry)
        self.assertTrue(t["is_live"])
        self.assertTrue(t["live"])  # legacy field kept
        self.assertEqual(t["duration_str"], "∞")


class TestYtdlpProgressLine(unittest.TestCase):
    def test_standard_line(self):
        p = ytdlp_mod.parse_ytdlp_progress_line(
            "[download]   3.2% of  10.50MiB at  500.00KiB/s ETA 00:20")
        self.assertIsNotNone(p)
        self.assertAlmostEqual(p["percentage"], 3.2)
        self.assertEqual(p["total_bytes"], int(10.50 * 1024 * 1024))
        self.assertEqual(p["done_bytes"],
                         int(10.50 * 1024 * 1024 * 0.032))
        self.assertEqual(p["speed_bytes_per_sec"], int(500.00 * 1024))
        self.assertEqual(p["eta_sec"], 20)

    def test_complete_line(self):
        p = ytdlp_mod.parse_ytdlp_progress_line(
            "[download] 100% of  4.56MiB in 00:03")
        self.assertAlmostEqual(p["percentage"], 100.0)
        self.assertEqual(p["total_bytes"], int(4.56 * 1024 * 1024))
        self.assertEqual(p["speed_bytes_per_sec"], 0)
        self.assertIsNone(p["eta_sec"])

    def test_unknown_total_and_hour_eta(self):
        p = ytdlp_mod.parse_ytdlp_progress_line(
            "[download]  45.0% of ~  5.00MiB at  1.00MiB/s ETA 01:02:03")
        self.assertAlmostEqual(p["percentage"], 45.0)
        self.assertEqual(p["eta_sec"], 3723)

    def test_non_progress_lines(self):
        for line in (
            "[download] Destination: /tmp/foo.mp3",
            "[ExtractAudio] Destination: /tmp/foo.mp3",
            "[youtube] Extracting URL: https://example.invalid",
            "",
            "[download]  12.3% of 4.56MiB at 1.23MiB/s ETA 00:03 "
            "(frag 4/10)",  # fragment lines still parse (same prefix)
        ):
            if "frag" in line:
                self.assertIsNotNone(
                    ytdlp_mod.parse_ytdlp_progress_line(line))
            else:
                self.assertIsNone(
                    ytdlp_mod.parse_ytdlp_progress_line(line), line)


class TestHumanizeFailure(unittest.TestCase):
    def test_all_mapped_needles(self):
        cases = {
            "transfer ended in state 'Completed, QueuedRemotely'":
                "waiting in the peer queue expired",
            "transfer ended in state 'Completed, ClientSide, Transfer'":
                "transfer timed out",
            "transfer ended in state 'Completed, TimedOut'":
                "peer accepted but never sent the file",
            "peer Rejected the transfer": "peer rejected the transfer",
            "transfer was Cancelled": "transfer was cancelled",
            "state 'Completed, Errored'": "transfer failed on the peer's side",
            "slskd refused enqueue from alice":
                "peer refused the download request",
            "could not locate downloaded file for x":
                "transfer finished but the file never appeared",
            "all matching peers already failed this album":
                "every matching peer had already failed on this album",
            "no peer returned a usable file": "all matching peers failed",
        }
        for raw, want in cases.items():
            self.assertEqual(progress_mod.humanize_failure(raw), want, raw)

    def test_passthrough_and_empty(self):
        self.assertEqual(
            progress_mod.humanize_failure("nothing found on Soulseek"),
            "nothing found on Soulseek")
        self.assertEqual(progress_mod.humanize_failure(""), "unknown error")
        self.assertEqual(progress_mod.humanize_failure(None), "unknown error")


class TestRegistry(unittest.TestCase):
    def setUp(self):
        self.reg = progress_mod.ProgressRegistry()

    def test_snapshot_shape(self):
        run = self.reg.new_run("youtube", "Kesariya",
                               per_track_titles=["Kesariya"])
        snap = self.reg.snapshot(run.run_id)
        # Every IPC-4 field, exact names.
        self.assertEqual(set(snap), {
            "run_id", "percentage", "done_bytes", "total_bytes",
            "speed_bytes_per_sec", "eta_sec", "elapsed_sec", "phase",
            "per_track", "lossy_available", "lossy_summary"})
        self.assertEqual(snap["run_id"], run.run_id)
        self.assertEqual(snap["phase"], "searching")
        self.assertEqual(snap["percentage"], 0)
        self.assertEqual(snap["done_bytes"], 0)
        self.assertIsNone(snap["total_bytes"])
        self.assertIsNone(snap["eta_sec"])
        self.assertEqual(snap["per_track"], [
            {"title": "Kesariya", "state": "pending", "pct": 0.0,
             "peer": None, "reason": None}])
        self.assertFalse(snap["lossy_available"])
        self.assertIsNone(snap["lossy_summary"])
        self.assertRegex(run.run_id, r"^[0-9a-f]{12}$")

    def test_update_and_track_and_finish(self):
        run = self.reg.new_run("soulseek", "Album",
                               per_track_titles=["A", "B"])
        self.reg.update(run.run_id, phase="downloading", percentage=42.5,
                        done_bytes=1000, total_bytes=2000,
                        speed_bytes_per_sec=512, eta_sec=7)
        self.reg.set_track(run.run_id, 0, state="downloading", pct=42.5,
                           peer="alice")
        self.reg.set_track(run.run_id, 1, state="failed",
                           reason="transfer ended in state 'Completed, TimedOut'")
        snap = self.reg.snapshot(run.run_id)
        self.assertEqual(snap["phase"], "downloading")
        self.assertEqual(snap["percentage"], 42.5)
        self.assertEqual(snap["per_track"][0]["peer"], "alice")
        # The raw slskd reason is humanised on the checklist.
        self.assertEqual(snap["per_track"][1]["reason"],
                         "peer accepted but never sent the file")
        # set_track out of range is a no-op, never an IndexError.
        self.reg.set_track(run.run_id, 99, state="done")
        self.reg.finish(run.run_id, "done")
        snap = self.reg.snapshot(run.run_id)
        self.assertEqual(snap["phase"], "done")
        self.assertEqual(snap["percentage"], 100.0)

    def test_finish_failed_humanises_error(self):
        run = self.reg.new_run("youtube", "x")
        self.reg.finish(run.run_id, "failed",
                        error="transfer ended in state 'Completed, Rejected'")
        snap = self.reg.snapshot(run.run_id)
        self.assertEqual(snap["phase"], "failed")
        self.assertEqual(run.error, "peer rejected the transfer")

    def test_unknown_run_is_none(self):
        self.assertIsNone(self.reg.snapshot("deadbeefcafe"))

    def test_finished_cap_evicts_oldest_only(self):
        live = self.reg.new_run("youtube", "live")
        for i in range(205):
            r = self.reg.new_run("youtube", "old-%d" % i)
            self.reg.finish(r.run_id, "done")
        # The live run survives; the oldest finished runs are evicted.
        self.assertIsNotNone(self.reg.get(live.run_id))
        self.assertEqual(len(self.reg._runs), 201)  # 200 finished + live

    def test_concurrent_updates_stay_consistent(self):
        run = self.reg.new_run("youtube", "x")
        def _pump(n):
            for i in range(50):
                self.reg.update(run.run_id, percentage=float(i),
                                done_bytes=i * 100)
                self.reg.snapshot(run.run_id)
        threads = [threading.Thread(target=_pump, args=(i,))
                   for i in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        snap = self.reg.snapshot(run.run_id)
        self.assertEqual(snap["run_id"], run.run_id)
        self.assertTrue(0 <= snap["percentage"] < 50)


class TestSoulseekRunFolding(unittest.TestCase):
    """The slskd callbacks fold real numbers into the run (mocked
    slskd — no daemon, no network)."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.reg = progress_mod.ProgressRegistry()
        # A real file for find_local_file to "locate".
        self.staged = os.path.join(self.tmp, "staged.flac")
        with open(self.staged, "wb") as fh:
            fh.write(b"FLACBYTES" * 10)

    def _candidate(self):
        return SearchResult("fastpeer",
                            "Music\\Daft Punk - Discovery\\01 One More Time.flac",
                            900, bit_depth=16, sample_rate=44100)

    def _patch_slskd(self, progress_events):
        """Stub the slskd client: enqueue ok, wait_for_files replays
        the canned progress events, then succeeds."""
        def _wait(username, target_filenames, timeout_secs=600,
                  poll_interval=2.0, progress_cb=None, state_cb=None,
                  stall_secs=None):
            for ev in progress_events:
                if progress_cb:
                    progress_cb(*ev)
            if state_cb:
                state_cb(target_filenames[0], "Queued, Remotely", 3, 12)
            return {target_filenames[0]: "Completed, Succeeded"}
        patches = [
            mock.patch.object(slskd_client, "get_active_download_state",
                              return_value=None),
            mock.patch.object(slskd_client, "enqueue", return_value=True),
            mock.patch.object(slskd_client, "wait_for_files",
                              side_effect=_wait),
            mock.patch.object(slskd_client, "find_local_file",
                              return_value=self.staged),
            mock.patch.object(slskd_client, "slskd_download_dir",
                              return_value=self.tmp),
            mock.patch.object(slskd_client, "remove_completed_downloads",
                              return_value=None),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def test_single_track_progress_folds_into_run(self):
        run = self.reg.new_run("soulseek", "One More Time",
                               per_track_titles=["Daft Punk - One More Time"])
        self._patch_slskd([
            ("f", 25.0, "In Progress", 512000, 225, 900),
            ("f", 75.0, "In Progress", 512000, 675, 900),
        ])
        path, downloaded, fmt, peer = slskd_dl.download_single_track(
            {"title": "One More Time", "artist": "Daft Punk"},
            os.path.join(self.tmp, "out"), precomputed_candidates=[self._candidate()],
            run=run)
        self.assertTrue(downloaded)
        self.assertEqual(peer, "fastpeer")
        snap = self.reg.snapshot(run.run_id)
        self.assertEqual(snap["phase"], "downloading")
        self.assertEqual(snap["percentage"], 100.0)
        # done/total are the real bytes on disk (the staged file is
        # 90 bytes; the 900 in the canned slskd events was the
        # transfer's reported size, confirmed by the file on landing).
        self.assertEqual(snap["done_bytes"], 90)
        self.assertEqual(snap["total_bytes"], 90)
        entry = snap["per_track"][0]
        self.assertEqual(entry["state"], "done")
        self.assertEqual(entry["pct"], 100.0)
        self.assertEqual(entry["peer"], "fastpeer")

    def test_no_flac_but_lossy_sets_lossy_fields(self):
        run = self.reg.new_run("soulseek", "Rare Track",
                               per_track_titles=["Nobody - Rare Track"])
        lossy = [self._candidate(), self._candidate()]
        def _find(track, accept_lossy=False, diag=None, **kw):
            if diag is not None:
                diag["lossy_dropped"] = True
            return (None, [])
        with mock.patch.object(slskd_dl, "find_track",
                              side_effect=_find), \
             mock.patch.object(slskd_dl, "find_lossy_candidates",
                              return_value=lossy):
            with self.assertRaises(RuntimeError):
                slskd_dl.download_single_track(
                    {"title": "Rare Track", "artist": "Nobody"},
                    os.path.join(self.tmp, "out"), run=run)
        snap = self.reg.snapshot(run.run_id)
        self.assertTrue(snap["lossy_available"])
        self.assertIn("2 mp3/m4a copies", snap["lossy_summary"])
        self.assertEqual(snap["per_track"][0]["state"], "failed")
        self.assertIn("only lossy copies found",
                      snap["per_track"][0]["reason"])


class TestServerProgressRoute(unittest.TestCase):
    """The HTTP layer: run_id in download responses, GET progress."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.config = server_mod.Config()
        self.config.port = 0
        self.config.token = None
        self.config.dl_dir = self.tmp
        self.server = ThreadingHTTPServer(
            ("127.0.0.1", 0), server_mod.make_handler(self.config))
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()

    def _req(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(
            "http://127.0.0.1:%d%s" % (self.port, path), data=data,
            method=method,
            headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                return resp.status, json.loads(resp.read())
        except urllib.error.HTTPError as exc:
            return exc.code, json.loads(exc.read())

    def test_progress_unknown_run_404(self):
        status, payload = self._req(
            "GET", "/v1/download/progress?run_id=deadbeefcafe")
        self.assertEqual(status, 404)
        self.assertEqual(payload["error"]["code"], "not_found")

    def test_progress_known_run(self):
        run = progress_mod.REGISTRY.new_run("youtube", "t")
        run.update(phase="downloading", percentage=55.5)
        status, payload = self._req(
            "GET", "/v1/download/progress?run_id=%s" % run.run_id)
        self.assertEqual(status, 200)
        self.assertEqual(payload["data"]["run_id"], run.run_id)
        self.assertEqual(payload["data"]["percentage"], 55.5)
        self.assertEqual(payload["data"]["phase"], "downloading")

    def test_youtube_download_returns_run_id_and_finishes_done(self):
        staged = os.path.join(self.tmp, "song.mp3")
        with open(staged, "wb") as fh:
            fh.write(b"MP3" * 100)
        track = {"id": "yt:x", "title": "X", "source": "youtube"}
        with mock.patch.object(
                ytdlp_mod, "download_youtube_audio",
                return_value=(staged, track)) as dl:
            status, payload = self._req(
                "POST", "/v1/download/youtube", {"query": "kesariya"})
        self.assertEqual(status, 200, payload)
        run_id = payload["data"]["run_id"]
        self.assertRegex(run_id, r"^[0-9a-f]{12}$")
        dl.assert_called_once()
        snap = progress_mod.REGISTRY.snapshot(run_id)
        self.assertEqual(snap["phase"], "done")
        self.assertEqual(snap["percentage"], 100.0)

    def test_failed_download_marks_run_failed(self):
        with mock.patch.object(
                ytdlp_mod, "download_youtube_audio",
                side_effect=Exception("boom")):
            status, payload = self._req(
                "POST", "/v1/download/youtube", {"query": "kesariya"})
        self.assertEqual(status, 500, payload)
        # The run_id is not in a failed response (no data envelope),
        # but the run itself must be failed, not stuck downloading.
        # Find it: the registry's latest finished run for this title.
        runs = [r for r in progress_mod.REGISTRY._runs.values()
                if r.title == "kesariya"]
        self.assertTrue(runs)
        self.assertEqual(runs[-1].phase, "failed")


if __name__ == "__main__":
    unittest.main()
