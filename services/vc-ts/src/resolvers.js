// Track resolvers for vc-ts. Re-implemented from the behaviour of the
// upstream refs (never copied):
//   TGVCBot  apps/tgvcbot/src/providers/youtube.ts  (youtube-sr search)
//   TGVCBot  apps/tgvcbot/src/providers/jiosaavn.ts (jsvn-tgvc.vercel.app)
//   lemonic  modules/lemonic.ts                     (LEMONIC_API raw track)
//
// YouTube is resolved with yt-dlp (same approach as vc-go and the
// downloaders service). JioSaavn goes through TGVCBot's public proxy
// endpoint jsvn-tgvc.vercel.app (saavn.dev is unreachable from this
// sandbox). Lemonic needs LEMONIC_API_KEY (+ optional LEMONIC_API base
// URL); without it every Lemonic call honestly returns
// engine_unavailable — results are never faked.

"use strict";

const { execFile } = require("child_process");
const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

const YT_DLP_DEFAULT = path.join(
  __dirname, "..", "..", "downloaders", ".venv", "bin", "yt-dlp"
);

// Parse a duration in any shape the upstreams use: seconds as a
// number (saavn.dev, yt-dlp), seconds as a numeric string, or a
// "m:ss" / "h:mm:ss" clock string (jsvn-tgvc /song returns "4:28").
// Returns integer seconds, or 0 when unparseable. The old code did
// Number("4:28") which is NaN -> 0, so every JioSaavn track lost its
// duration and the auto-advance ticker never fired for them.
function parseDuration(value) {
  if (value === undefined || value === null || value === "") return 0;
  if (typeof value === "number") return Number.isFinite(value) ? Math.round(value) : 0;
  const s = String(value).trim();
  if (/^\d+(\.\d+)?$/.test(s)) return Math.round(Number(s));
  const parts = s.split(":").map((p) => Number(p));
  if (parts.length < 2 || parts.length > 3 || parts.some((p) => !Number.isFinite(p) || p < 0)) return 0;
  if (parts.length === 2) return Math.round(parts[0] * 60 + parts[1]);
  return Math.round(parts[0] * 3600 + parts[1] * 60 + parts[2]);
}

function ytDlpPath() {
  if (process.env.YT_DLP) return process.env.YT_DLP;
  try {
    fs.accessSync(YT_DLP_DEFAULT, fs.constants.X_OK);
    return YT_DLP_DEFAULT;
  } catch {
    return "yt-dlp"; // fall back to PATH; exec failure is handled by caller
  }
}

function ytDlpAvailable() {
  try {
    fs.accessSync(ytDlpPath(), fs.constants.X_OK);
    return true;
  } catch {
    return false;
  }
}

// Error type that maps to the contract's engine_unavailable code.
class EngineUnavailable extends Error {
  constructor(source, message) {
    super(message);
    this.code = "engine_unavailable";
    this.source = source;
  }
}

function runYtDlp(args, timeoutMs = 30000) {
  return new Promise((resolve) => {
    execFile(
      ytDlpPath(), args,
      { timeout: timeoutMs, maxBuffer: 16 * 1024 * 1024 },
      (err, stdout) => {
        if (err) {
          resolve({ ok: false, error: (err.message || String(err)).slice(0, 300) });
          return;
        }
        // -j on a single video prints ONE JSON object; -j on a search
        // (ytsearchN:) prints one JSON object PER LINE (JSONL). Handle both.
        const entries = [];
        const text = String(stdout || "").trim();
        if (!text) return resolve({ ok: false, error: "yt-dlp printed nothing" });
        try {
          entries.push(JSON.parse(text));
        } catch {
          for (const line of text.split("\n")) {
            const l = line.trim();
            if (!l) continue;
            try { entries.push(JSON.parse(l)); } catch { /* skip bad line */ }
          }
        }
        if (!entries.length) {
          return resolve({ ok: false, error: "yt-dlp output was not JSON" });
        }
        resolve({ ok: true, json: entries[0], entries });
      }
    );
  });
}

const YT_URL_RE =
  /(?:youtube\.com\/(?:watch\?[^#\s]*v=|shorts\/|embed\/|v\/)|youtu\.be\/)([A-Za-z0-9_-]{11})/;

function extractYouTubeId(text) {
  const m = String(text || "").match(YT_URL_RE);
  return m ? m[1] : null;
}

function ytTrackFromInfo(info, fallbackUrl) {
  return {
    id: `yt:${info.id}`,
    title: info.title || "Unknown title",
    artist: info.uploader || info.channel || "Unknown",
    duration_sec: Math.round(Number(info.duration) || 0),
    url: info.webpage_url || fallbackUrl || `https://www.youtube.com/watch?v=${info.id}`,
    thumbnail: info.thumbnail || null,
    source: "youtube",
  };
}

function degradedYouTubeTrack(query, videoId) {
  // yt-dlp missing/failed/rate-limited (YouTube 429s the sandbox egress
  // IP): keep a placeholder so play/queue still work, and say so.
  if (videoId) {
    return {
      id: `yt:${videoId}`,
      title: `YouTube video ${videoId}`,
      artist: "Unknown",
      duration_sec: 0,
      url: `https://www.youtube.com/watch?v=${videoId}`,
      thumbnail: null,
      source: "youtube",
      degraded: true,
      resolve_note: "yt-dlp resolve failed (missing or YouTube 429); degraded placeholder",
    };
  }
  const hash = crypto.createHash("sha1").update(String(query)).digest("hex").slice(0, 16);
  return {
    id: `query:${hash}`,
    title: String(query),
    artist: "Unknown",
    duration_sec: 0,
    url: null,
    thumbnail: null,
    source: "youtube",
    degraded: true,
    resolve_note: "yt-dlp search failed (missing or YouTube 429); degraded placeholder",
  };
}

// Resolve a YouTube URL or a free-text query to one Track.
async function resolveYouTube(query) {
  const videoId = extractYouTubeId(query);
  if (videoId) {
    const url = `https://www.youtube.com/watch?v=${videoId}`;
    const r = await runYtDlp(["-j", "--no-playlist", url]);
    if (r.ok && r.json) return { track: ytTrackFromInfo(r.json, url), degraded: false };
    return { track: degradedYouTubeTrack(query, videoId), degraded: true, error: r.error };
  }
  const r = await runYtDlp(["-j", "--no-playlist", `ytsearch1:${query}`]);
  if (r.ok && r.json) return { track: ytTrackFromInfo(r.json, null), degraded: false };
  return { track: degradedYouTubeTrack(query, null), degraded: true, error: r.error };
}

// Search YouTube, up to `limit` tracks. On yt-dlp failure returns an
// empty list plus the error instead of inventing results.
async function searchYouTube(query, limit = 5) {
  const n = Math.max(1, Math.min(10, Number(limit) || 5));
  const r = await runYtDlp(["-j", "--no-playlist", `ytsearch${n}:${query}`]);
  if (r.ok && r.entries) {
    return { results: r.entries.filter(Boolean).map((e) => ytTrackFromInfo(e, null)), error: null };
  }
  return { results: [], error: r.error || "yt-dlp search failed" };
}

// --- JioSaavn (TGVCBot provider shape) -------------------------------------
// TGVCBot used https://jsvn-tgvc.vercel.app/search?query= and /song?id=.
// saavn.dev (the other public API) is unreachable from this sandbox.

const JIOSAAVN_BASES = [
  "https://jsvn-tgvc.vercel.app",
  "https://saavn.dev/api",
];

async function fetchJson(url, timeoutMs = 10000) {
  const res = await fetch(url, { signal: AbortSignal.timeout(timeoutMs) });
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return res.json();
}

function jiosaavnTrack(r) {
  const singers = r.more_info?.singers || r.primary_artists || r.singers || "Unknown";
  const t = {
    id: `jsvn:${r.id}`,
    title: String(r.title || r.song || "Unknown").replace(/&quot;/g, '"'),
    artist: singers,
    duration_sec: parseDuration(r.more_info?.duration ?? r.duration),
    url: r.perma_url || r.url || null,
    thumbnail: r.image || null,
    source: "jiosaavn",
  };
  if (r.media_url || r.media_urls) {
    t.stream_url = r.media_urls?.["96_KBPS"] || r.media_url;
  }
  return t;
}

// The search payload carries no duration and no media URL (see
// phase4/deepread-tsjs.md §6). Enrich each result with one parallel
// /song?id= lookup, which returns duration ("m:ss") + media_urls.
// Lookups run concurrently, bounded by the result count; a failed
// lookup leaves that track's duration at 0 and records
// duration_source instead of inventing a value.
async function enrichJioSaavnDurations(results) {
  await Promise.all(
    results.map(async (track) => {
      if (track.duration_sec > 0) return;
      const id = track.id.replace(/^jsvn:/, "");
      try {
        const full = await resolveJioSaavnSong(id);
        track.duration_sec = full.duration_sec;
        track.stream_url = full.stream_url || track.stream_url;
        track.duration_source = "song-lookup";
      } catch {
        track.duration_source = "unknown";
      }
    })
  );
  return results;
}

async function searchJioSaavn(query, limit = 5) {
  const errors = [];
  for (const base of JIOSAAVN_BASES) {
    try {
      const url =
        base === "https://saavn.dev/api"
          ? `${base}/search/songs?query=${encodeURIComponent(query)}`
          : `${base}/search?query=${encodeURIComponent(query)}`;
      const data = await fetchJson(url);
      const raw = data.results || (data.data && data.data.results) || [];
      const results = raw.slice(0, limit).map(jiosaavnTrack);
      if (results.length) return { results: await enrichJioSaavnDurations(results), error: null };
      return { results: [], error: null }; // API answered, no matches
    } catch (e) {
      errors.push(`${base}: ${e.message}`);
    }
  }
  throw new EngineUnavailable(
    "jiosaavn",
    `JioSaavn search unavailable (tried ${JIOSAAVN_BASES.join(", ")}): ${errors.join("; ")}`
  );
}

async function resolveJioSaavnSong(id) {
  const errors = [];
  for (const base of JIOSAAVN_BASES) {
    try {
      const url =
        base === "https://saavn.dev/api"
          ? `${base}/songs/${encodeURIComponent(id)}`
          : `${base}/song?id=${encodeURIComponent(id)}`;
      const data = await fetchJson(url);
      const song = data.data && data.data[0] ? data.data[0] : data;
      const t = jiosaavnTrack({
        ...song,
        title: song.song || song.title,
        singers: song.singers || song.primary_artists,
        duration: song.duration,
      });
      if (song.media_url || song.media_urls) t.stream_url = song.media_urls?.["96_KBPS"] || song.media_url;
      return t;
    } catch (e) {
      errors.push(`${base}: ${e.message}`);
    }
  }
  throw new EngineUnavailable("jiosaavn", `JioSaavn song lookup failed: ${errors.join("; ")}`);
}

// --- Lemonic ---------------------------------------------------------------
// Upstream truth (phase4/deepread-tsjs.md §8): lemonic-telegram-bot's
// modules/lemonic.ts does an UNAUTHENTICATED fetch to
// ${LEMONIC_API}/api/raw/track/${id} — no API key exists upstream, and
// search upstream is Spotify client-credentials search, not a Lemonic
// endpoint. So: raw-track and /api/search are tried unauthenticated
// first; when LEMONIC_API_KEY is set, requests additionally carry
// `Authorization: Bearer <key>` (keyed path). Search falls back to
// Spotify client-credentials search when SPOTIFY_CLIENT_ID +
// SPOTIFY_CLIENT_SECRET are set. Only when every fallback is
// unavailable or fails do we return engine_unavailable — and the error
// names everything that was tried, so a caller can tell a missing
// credential from a dead API.

function lemonicBase() {
  return (process.env.LEMONIC_API || "https://api.lemonic.xyz").replace(/\/$/, "");
}

function lemonicConfigured() {
  return Boolean(process.env.LEMONIC_API_KEY);
}

// "keyed" | "unauthenticated" | "spotify" | "none": which Lemonic path
// can actually run. Unauthenticated raw-track is upstream behaviour,
// so the API being reachable is enough for "unauthenticated".
function lemonicMode() {
  if (lemonicConfigured()) return "keyed";
  if (process.env.SPOTIFY_CLIENT_ID && process.env.SPOTIFY_CLIENT_SECRET) return "spotify";
  return "unauthenticated";
}

function lemonicHeaders() {
  return lemonicConfigured()
    ? { Authorization: `Bearer ${process.env.LEMONIC_API_KEY}` }
    : {};
}

async function lemonicFetch(path) {
  const res = await fetch(`${lemonicBase()}${path}`, {
    headers: lemonicHeaders(),
    signal: AbortSignal.timeout(10000),
  });
  if (!res.ok) {
    throw new EngineUnavailable("lemonic", `Lemonic API returned HTTP ${res.status} for ${path}`);
  }
  return res.json();
}

function lemonicTrack(r) {
  // duration may arrive as seconds, "m:ss", or milliseconds.
  const durationSec = r.duration_ms
    ? Math.round(Number(r.duration_ms) / 1000)
    : parseDuration(r.duration ?? r.duration_sec);
  return {
    id: `lemonic:${r.id || r.track_id}`,
    title: r.title || r.name || "Unknown",
    artist: r.artist || (Array.isArray(r.artists) ? r.artists.join(", ") : "Unknown"),
    duration_sec: durationSec,
    url: r.url || r.perma_url || null,
    thumbnail: r.thumbnail || r.image || null,
    source: "lemonic",
  };
}

// --- Spotify client-credentials search (lemonic's own search path) --------
// lemonic-telegram-bot modules/spotify.ts: SpotifyApi.withClientCredentials.
// Token is cached in memory until expiry; no user OAuth involved.
let spotifyTokenCache = null; // { token, expires_at_ms }

async function spotifyClientToken() {
  const id = process.env.SPOTIFY_CLIENT_ID;
  const secret = process.env.SPOTIFY_CLIENT_SECRET;
  if (!id || !secret) return null;
  if (spotifyTokenCache && Date.now() < spotifyTokenCache.expires_at_ms - 60000) {
    return spotifyTokenCache.token;
  }
  const res = await fetch("https://accounts.spotify.com/api/token", {
    method: "POST",
    headers: {
      "Content-Type": "application/x-www-form-urlencoded",
      Authorization: "Basic " + Buffer.from(`${id}:${secret}`).toString("base64"),
    },
    body: "grant_type=client_credentials",
    signal: AbortSignal.timeout(10000),
  });
  if (!res.ok) throw new Error(`Spotify token endpoint returned HTTP ${res.status}`);
  const data = await res.json();
  spotifyTokenCache = {
    token: data.access_token,
    expires_at_ms: Date.now() + (Number(data.expires_in) || 3600) * 1000,
  };
  return spotifyTokenCache.token;
}

async function spotifySearchTracks(query, limit = 5) {
  const token = await spotifyClientToken();
  if (!token) return null;
  const url = `https://api.spotify.com/v1/search?${new URLSearchParams({
    q: query,
    type: "track",
    market: "IN",
    limit: String(limit),
  })}`;
  const res = await fetch(url, {
    headers: { Authorization: `Bearer ${token}` },
    signal: AbortSignal.timeout(10000),
  });
  if (!res.ok) throw new Error(`Spotify search returned HTTP ${res.status}`);
  const data = await res.json();
  const items = data.tracks?.items || [];
  // Map to Lemonic-shaped tracks: the id is the Spotify id, which is
  // exactly what lemonic's /api/raw/track/<spotify-id> expects — the
  // same flow as the upstream bot (Spotify search -> Lemonic raw).
  return items.map((t) => ({
    id: `lemonic:${t.id}`,
    title: t.name || "Unknown",
    artist: (t.artists || []).map((a) => a.name).join(", ") || "Unknown",
    duration_sec: Math.round(Number(t.duration_ms || 0) / 1000),
    url: t.external_urls?.spotify || null,
    thumbnail: t.album?.images?.[0]?.url || null,
    source: "lemonic",
    via: "spotify-search",
  }));
}

async function searchLemonic(query, limit = 5) {
  const tried = [];
  // 1) The Lemonic API's own search endpoint (unauthenticated, or
  //    Bearer when a key is set). Upstream has no such call, but the
  //    endpoint exists on the API; try it first.
  try {
    const data = await lemonicFetch(`/api/search?query=${encodeURIComponent(query)}&limit=${limit}`);
    const raw = data.results || data.tracks || data.data || [];
    if (!Array.isArray(raw)) {
      throw new EngineUnavailable("lemonic", "Lemonic search returned an unexpected response shape");
    }
    if (raw.length) return { results: raw.slice(0, limit).map(lemonicTrack), error: null, via: lemonicConfigured() ? "lemonic-keyed" : "lemonic" };
    return { results: [], error: null, via: "lemonic" }; // answered, no matches
  } catch (e) {
    tried.push(`lemonic /api/search: ${e.message}`);
  }
  // 2) Spotify client-credentials search — lemonic-telegram-bot's own
  //    search path (modules/spotify.ts + createSearchResults.ts).
  try {
    const results = await spotifySearchTracks(query, limit);
    if (results) {
      return { results, error: null, via: "spotify-search" };
    }
    tried.push("spotify search: SPOTIFY_CLIENT_ID/SPOTIFY_CLIENT_SECRET not set");
  } catch (e) {
    tried.push(`spotify search: ${e.message}`);
  }
  throw new EngineUnavailable(
    "lemonic",
    `Lemonic search unavailable — every fallback failed: ${tried.join("; ")}`
  );
}

async function lemonicRawTrack(id) {
  // Mirrors modules/lemonic.ts: GET ${LEMONIC_API}/api/raw/track/${id}
  // (unauthenticated upstream; Bearer added only when a key is set).
  const data = await lemonicFetch(`/api/raw/track/${encodeURIComponent(id)}`);
  const raw = data.data || data;
  if (!raw || (!raw.id && !raw.title)) {
    throw new EngineUnavailable("lemonic", `Lemonic has no raw track for id ${id}`);
  }
  const track = lemonicTrack({ ...raw, id: raw.id || id });
  return { track, raw };
}

// --- Lyrics --------------------------------------------------------------
// remix handlers/lyrics.ts fetches lyrics for the now-playing title via
// lyrics-searcher (Genius). No npm deps are allowed here, so the
// keyless LRCLib API is used instead (same one spotify-rust uses):
// GET https://lrclib.net/api/search?q=<title artist>. Returns plain
// lyrics, or synced (timestamped) lyrics when available. JioSaavn
// cannot supply lyric text (its /song payload has has_lyrics:true but
// lyrics:null). A miss is reported as null lyrics, never invented.

async function fetchLyrics(title, artist) {
  // JioSaavn artist strings are comma-joined lists ("Pritam, Arijit
  // Singh, Amitabh Bhattacharya"); LRCLib answers far more reliably
  // with just the first artist, so trim to that.
  const firstArtist = String(artist || "").split(",")[0].trim();
  const q = [title, firstArtist].filter(Boolean).join(" ").trim();
  if (!q) return null;
  // LRCLib intermittently 503s under load; one retry keeps a transient
  // blip from turning a real miss into an outage report.
  let lastErr = null;
  for (let attempt = 0; attempt < 2; attempt++) {
    try {
      const res = await fetch(
        `https://lrclib.net/api/search?${new URLSearchParams({ q })}`,
        { signal: AbortSignal.timeout(10000) }
      );
      if (!res.ok) throw new EngineUnavailable("lyrics", `LRCLib returned HTTP ${res.status}`);
      const data = await res.json();
      if (!Array.isArray(data) || !data.length) return null;
      // Prefer an exact-ish title match, else the first hit.
      const norm = (s) => String(s || "").toLowerCase().replace(/[^a-z0-9]+/g, " ").trim();
      const hit =
        data.find((d) => norm(d.trackName) === norm(title)) ||
        data.find((d) => norm(d.name) === norm(title)) ||
        data[0];
      const lyrics = hit.syncedLyrics || hit.plainLyrics || null;
      if (!lyrics) return null;
      return {
        title: hit.trackName || hit.name || title,
        artist: hit.artistName || artist || "Unknown",
        lyrics,
        synced: Boolean(hit.syncedLyrics),
        source: "lrclib",
        instrumental: Boolean(hit.instrumental),
      };
    } catch (e) {
      lastErr = e;
      if (attempt === 0) await new Promise((r) => setTimeout(r, 500));
    }
  }
  throw lastErr;
}

// --- Radio / raw-stream tracks -------------------------------------------
// A raw http(s) audio URL (TGVCBot /radio, openradiobot raw streams) is
// a track in its own right: no yt-dlp, no search. duration_sec stays 0
// (endless stream) so the auto-advance ticker never fires for it —
// exactly how TGVCBot treats radio (duration: '').
function radioTrackFromUrl(url, stationName) {
  const hash = crypto.createHash("sha1").update(url).digest("hex").slice(0, 12);
  return {
    id: `radio:${hash}`,
    title: stationName || "Radio stream",
    artist: "live stream",
    duration_sec: 0,
    url,
    stream_url: url,
    thumbnail: null,
    source: "radio",
  };
}

module.exports = {
  EngineUnavailable,
  parseDuration,
  ytDlpPath,
  ytDlpAvailable,
  extractYouTubeId,
  resolveYouTube,
  searchYouTube,
  searchJioSaavn,
  resolveJioSaavnSong,
  lemonicConfigured,
  lemonicMode,
  lemonicBase,
  searchLemonic,
  lemonicRawTrack,
  fetchLyrics,
  radioTrackFromUrl,
};
