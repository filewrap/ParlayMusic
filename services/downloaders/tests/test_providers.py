"""downloaders provider tests (Worker D, Phase 4).

Run offline (unit) tests:
    .venv/bin/python -m unittest discover -s tests -v
Run live tests too (real downloads; needs network):
    PARLAY_LIVE_TEST=1 .venv/bin/python -m unittest discover -s tests -v
Navidrome live tests need a server as well:
    NAVIDROME_URL=... NAVIDROME_USER=... NAVIDROME_PASSWORD=... \
        PARLAY_LIVE_TEST=1 .venv/bin/python -m unittest discover -s tests -v
No credentials are read from disk anywhere; env vars only.
"""
import hashlib
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from src import deezer as deezer_mod
from src import navidrome as navidrome_mod
from src import spotify as spotify_mod
from src import spotify_api
from src import tiktok as tiktok_mod
from src.util import ApiError

LIVE = os.environ.get("PARLAY_LIVE_TEST") == "1"


def _dl_dir(test):
    d = tempfile.mkdtemp(prefix="parlay-dl-test-")
    test.addCleanup(lambda: __import__("shutil").rmtree(d, ignore_errors=True))
    return d


class TestSpotifyEmbedParsing(unittest.TestCase):
    SAMPLE_HTML = ('<html><script id="__NEXT_DATA__" type="application/json">'
                   '{"props":{"pageProps":{"state":{"data":{"entity":{'
                   '"name":"Today\\u2019s Top Hits","trackList":['
                   '{"uri":"spotify:track:7bxaFZ1O3cHkgLKMsdC3xR",'
                   '"title":"Loser","subtitle":"Tame Impala",'
                   '"duration":223069},'
                   '{"uri":"spotify:track:4uLU6hMCjMI75M1A2tKUQC",'
                   '"title":"Never Gonna Give You Up",'
                   '"subtitle":"Rick Astley","duration":213573}]}}}}}}'
                   '</script></html>')

    def test_entity_from_html(self):
        ent = spotify_mod._entity_from_html(self.SAMPLE_HTML)
        self.assertIsNotNone(ent)
        self.assertEqual(ent["name"], "Today’s Top Hits")
        self.assertEqual(len(ent["trackList"]), 2)

    def test_embed_track_list_mapping(self):
        ent = spotify_mod._entity_from_html(self.SAMPLE_HTML)
        tracks = []
        for item in ent["trackList"]:
            uri = item.get("uri") or ""
            tid = uri.rsplit(":", 1)[-1]
            tracks.append((tid, item.get("title"), item.get("subtitle"),
                           (item.get("duration") or 0) // 1000))
        self.assertEqual(tracks[0],
                         ("7bxaFZ1O3cHkgLKMsdC3xR", "Loser", "Tame Impala", 223))
        self.assertEqual(tracks[1][1], "Never Gonna Give You Up")

    def test_entity_from_html_garbage(self):
        self.assertIsNone(spotify_mod._entity_from_html("<html>no data</html>"))
        self.assertIsNone(spotify_mod._entity_from_html(
            '<script id="__NEXT_DATA__">not json</script>'))


class TestParsing(unittest.TestCase):
    def test_deezer_url_parse(self):
        self.assertEqual(
            deezer_mod.parse_deezer_url("https://www.deezer.com/track/3135556"),
            ("track", "3135556"))
        self.assertEqual(
            deezer_mod.parse_deezer_url("https://www.deezer.com/us/album/302127"),
            ("album", "302127"))
        self.assertIsNone(deezer_mod.parse_deezer_url("https://example.com/x"))
        self.assertIsNone(deezer_mod.parse_deezer_url("never gonna give you up"))

    def test_tiktok_url_parse(self):
        self.assertEqual(
            tiktok_mod.parse_tiktok_url(
                "https://www.tiktok.com/@musicpopularbr_/video/7643895444082281736"),
            "7643895444082281736")
        self.assertEqual(
            tiktok_mod.parse_tiktok_url("https://vm.tiktok.com/ZMabcdef/"),
            "ZMabcdef")
        self.assertIsNone(
            tiktok_mod.parse_tiktok_url("https://www.youtube.com/watch?v=x"))

    def test_spotify_url_parse(self):
        self.assertEqual(
            spotify_mod.parse_spotify_url(
                "https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC"),
            ("track", "4uLU6hMCjMI75M1A2tKUQC"))
        self.assertEqual(
            spotify_mod.parse_spotify_url(
                "https://open.spotify.com/playlist/37i9dQZF1DXcBWIGoYBM5M"),
            ("playlist", "37i9dQZF1DXcBWIGoYBM5M"))
        self.assertIsNone(spotify_mod.parse_spotify_url("https://example.com/"))


class TestSpotifyApiMapping(unittest.TestCase):
    def test_track_dict_from_api_shape(self):
        item = {
            "id": "4uLU6hMCjMI75M1A2tKUQC",
            "name": "Never Gonna Give You Up",
            "duration_ms": 213573,
            "artists": [{"name": "Rick Astley"}],
            "album": {"name": "Whenever You Need Somebody",
                      "images": [{"url": "https://x/big"}, {"url": "https://x/small"}]},
            "external_urls": {"spotify": "https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC"},
        }
        t = spotify_api._track_dict(item)
        self.assertEqual(t["id"], "4uLU6hMCjMI75M1A2tKUQC")
        self.assertEqual(t["title"], "Never Gonna Give You Up")
        self.assertEqual(t["artist"], "Rick Astley")
        self.assertEqual(t["duration_sec"], 213)
        self.assertEqual(t["thumbnail"], "https://x/small")
        self.assertEqual(t["album"], "Whenever You Need Somebody")

    def test_configured_needs_both_vars(self):
        # Env-dependent: only asserts the False side when creds absent.
        if not (os.environ.get("SPOTIFY_CLIENT_ID")
                and os.environ.get("SPOTIFY_CLIENT_SECRET")):
            self.assertFalse(spotify_api.configured())


class TestNavidromeAuth(unittest.TestCase):
    def test_token_auth_params_shape(self):
        navidrome_mod._AUTH_MODE["mode"] = "token"
        params = navidrome_mod._auth_params("demo", "demo")
        self.assertEqual(params["u"], "demo")
        self.assertEqual(len(params["s"]), 16)
        expected = hashlib.md5(("demo" + params["s"]).encode()).hexdigest()
        self.assertEqual(params["t"], expected)

    def test_missing_env_is_engine_unavailable(self):
        old = {k: os.environ.pop(k, None) for k in
               ("NAVIDROME_URL", "NAVIDROME_USER", "NAVIDROME_PASSWORD")}
        try:
            with self.assertRaises(ApiError) as ctx:
                navidrome_mod._env()
            self.assertEqual(ctx.exception.code, "engine_unavailable")
        finally:
            for k, v in old.items():
                if v is not None:
                    os.environ[k] = v


class TestDeezerMapping(unittest.TestCase):
    def test_track_from_api(self):
        d = {"id": 3135556, "title": "Harder, Better, Faster, Stronger",
             "duration": 224, "link": "https://www.deezer.com/track/3135556",
             "preview": "https://cdnt-preview.dzcdn.net/x.mp3",
             "artist": {"name": "Daft Punk"},
             "album": {"cover_xl": "https://cover/xl"}}
        t = deezer_mod._track_from_api(d)
        self.assertEqual(t["id"], "3135556")
        self.assertEqual(t["title"], "Harder, Better, Faster, Stronger")
        self.assertEqual(t["artist"], "Daft Punk")
        self.assertEqual(t["duration_sec"], 224)
        self.assertEqual(t["source"], "deezer")
        self.assertIn("30-second preview", t["note"])


class TestTikTokAudioPick(unittest.TestCase):
    def test_best_audio_url_prefers_mp3_audio_only(self):
        info = {"formats": [
            {"format_id": "download", "ext": "mp4", "acodec": "aac",
             "vcodec": "h264", "url": "https://cdn/video"},
            {"format_id": "h264", "ext": "mp4", "acodec": "aac",
             "vcodec": "h264", "url": "https://cdn/video2"},
            {"format_id": "audio", "ext": "mp3", "acodec": "mp3",
             "vcodec": "none", "abr": 128, "url": "https://cdn/audio.mp3"},
        ]}
        self.assertEqual(tiktok_mod._best_audio_url(info),
                         "https://cdn/audio.mp3")

    def test_best_audio_url_none_without_audio(self):
        info = {"formats": [{"format_id": "download", "ext": "mp4",
                             "acodec": "aac", "vcodec": "h264",
                             "url": "https://cdn/video"}]}
        self.assertIsNone(tiktok_mod._best_audio_url(info))


@unittest.skipUnless(LIVE, "live tests need PARLAY_LIVE_TEST=1")
class TestLiveDownloads(unittest.TestCase):
    def test_youtube_download_still_works(self):
        from src import providers
        out = providers.youtube_download(
            {"query": "rick astley never gonna give you up",
             "_dl_dir": _dl_dir(self)})
        self.assertTrue(os.path.getsize(out["file_path"]) > 100_000)
        self.assertTrue(out["file_path"].endswith(".mp3"))
        self.assertEqual(out["track"]["source"], "youtube")
        print("\nyoutube live OK: %s (%d bytes)"
              % (out["file_path"], out["size_bytes"]))

    def test_spotify_track_fallback_still_works(self):
        out = spotify_mod.download_spotify_track(
            spotify_mod.resolve_track(
                "https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC"),
            _dl_dir(self))
        path, track = out
        self.assertTrue(os.path.getsize(path) > 100_000)
        self.assertEqual(track["source"], "spotify")
        print("\nspotify track live OK: %s — %s (%d bytes)"
              % (track.get("artist"), track.get("title"),
                 os.path.getsize(path)))

    def test_spotify_playlist_resolves_real_track_list(self):
        tracks = spotify_mod.resolve_collection(
            "https://open.spotify.com/playlist/37i9dQZF1DXcBWIGoYBM5M")
        self.assertGreater(len(tracks), 5)
        self.assertTrue(all(t.get("title") for t in tracks))
        print("\nspotify playlist live OK: %d tracks, first: %s — %s"
              % (len(tracks), tracks[0].get("artist"),
                 tracks[0].get("title")))

    def test_deezer_search_and_preview_download(self):
        results = deezer_mod.search_deezer(
            "rick astley never gonna give you up", limit=3)
        self.assertGreater(len(results), 0)
        self.assertEqual(results[0]["source"], "deezer")
        path, track = deezer_mod.download_track_by_id(
            str(results[0]["id"]), _dl_dir(self))
        size = os.path.getsize(path)
        self.assertTrue(path.endswith(".mp3"))
        self.assertGreater(size, 10_000)
        self.assertIn("30-second preview", track["note"])
        print("\ndeezer live OK: %s — %s (%d bytes, preview)"
              % (track.get("artist"), track.get("title"), size))

    def test_deezer_album_zip(self):
        # Daft Punk — Discovery (Deezer album 302127), 14 tracks.
        out = deezer_mod.download_deezer(
            {"url": "https://www.deezer.com/album/302127",
             "_dl_dir": _dl_dir(self)})
        self.assertTrue(out["file_path"].endswith(".zip"))
        self.assertGreater(out["count"], 5)
        self.assertGreater(os.path.getsize(out["file_path"]), 100_000)
        print("\ndeezer album live OK: %d previews zipped (%d bytes)"
              % (out["count"], os.path.getsize(out["file_path"])))

    def test_tiktok_download(self):
        out = tiktok_mod.download_tiktok(
            {"url": "https://www.tiktok.com/@musicpopularbr_/video/7643895444082281736",
             "_dl_dir": _dl_dir(self)})
        size = os.path.getsize(out["file_path"])
        self.assertTrue(out["file_path"].endswith(".mp3"))
        self.assertGreater(size, 100_000)
        self.assertEqual(out["track"]["source"], "tiktok")
        print("\ntiktok live OK: %s (%d bytes, %s)"
              % (out["track"].get("title"), size,
                 out["track"].get("note", "")[:60]))

    @unittest.skipUnless(
        os.environ.get("NAVIDROME_URL")
        and os.environ.get("NAVIDROME_USER")
        and os.environ.get("NAVIDROME_PASSWORD"),
        "navidrome live tests need NAVIDROME_* env vars")
    def test_navidrome_ping_search_download(self):
        version = navidrome_mod.ping()
        self.assertTrue(version)
        songs = navidrome_mod.search_songs("Feijão", limit=3)
        self.assertGreater(len(songs), 0)
        path, track = navidrome_mod.download_song(songs[0], _dl_dir(self))
        size = os.path.getsize(path)
        self.assertGreater(size, 1_000_000)
        self.assertEqual(track["source"], "navidrome")
        print("\nnavidrome live OK: server %s, %s — %s (%d bytes)"
              % (version, track.get("artist"), track.get("title"), size))

    @unittest.skipUnless(
        spotify_api.configured(),
        "spotify API live tests need SPOTIFY_CLIENT_ID/SECRET in env")
    def test_spotify_api_real_track_and_playlist(self):
        track = spotify_api.get_track("4uLU6hMCjMI75M1A2tKUQC")
        self.assertIsNotNone(track)
        self.assertEqual(track["title"], "Never Gonna Give You Up")
        got = spotify_api.get_playlist("37i9dQZF1DXcBWIGoYBM5M")
        self.assertIsNotNone(got)
        name, tracks, total, truncated = got
        self.assertGreater(len(tracks), 5)
        print("\nspotify API live OK: track=%s; playlist %r: %d/%d tracks"
              % (track["title"], name, len(tracks), total))


if __name__ == "__main__":
    unittest.main()
