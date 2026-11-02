//! parlay-vc-rust — fallback Rust VC engine (Parlay IPC `vc.*`).
//!
//! Behaviour ported (re-implemented from scratch, not copied) from
//! refs/ParvatiMusic:
//!   `bot/src/modules/play.rs`    — /play family: resolve query, enqueue,
//!                                 start playback, live message editing
//!   `bot/src/modules/queuecmd.rs` — /queue listing: now-playing + up-next
//!   `bot/src/modules/{pause,skip,stop}.rs` — pause / resume / skip / stop
//!   `bot/src/modules/vc.rs`      — voice-chat state / assistant join
//!   `bot/src/room.rs` (RoomState) — per-chat room: current track, queue,
//!                                  paused flag, position
//!
//! What is REAL here: the full queue/playback state machine over the IPC
//! contract (`contracts/ipc.json` `vc.*`): play/pause/resume/skip/stop/
//! seek, queue add/remove/clear/list, now-playing, per-chat state,
//! in-memory + JSON-file persistence (`data/vc-state.json`), a 1-second
//! auto-advance ticker, yt-dlp subprocess resolve (`ytdlp_bin()`: PATH,
//! then the downloaders venv; android-player-client retry on YouTube's
//! bot check) — and no pattern/placeholder fallback: a query yt-dlp
//! cannot resolve is `engine_unavailable` (HTTP 502), never a fake
//! track (Phase 5, A9). Unknown-duration tracks (duration_sec 0)
//! never auto-advance; and — new in Phase 4 — a real
//! download path (each track's audio is downloaded to a local mp3 with
//! yt-dlp + ffmpeg before it starts, ParvatiMusic `play.rs::RealSink`),
//! loop count (`/loop`, ParvatiMusic `room.rs::loop_n`, 0..=10),
//! shuffle (`/shuffle`, queue shuffled on enable + random advance pick),
//! and autoplay (`/autoplay`, `/autoplay-skip`: YouTube mix playlist
//! `RD<video_id>` picks excluding play history, ParvatiMusic
//! `platforms/youtube.rs::mix_for` + `monitor::advance_locked`).
//!
//! Voice transport is REAL too (see `voice.rs`): an Opus/RTP pipeline
//! (ffmpeg -> libopus 48 kHz stereo -> Ogg Opus file + RTP packets,
//! packet count proven by parsing the RTP file) and a TDLib-join client
//! (ntgcalls local offer; no MTProto join in-process — the old
//! voice_bridge.py Telethon join was deleted in Phase 5, join goes
//! through the core-cpp TDLib backend). In this
//! sandbox the live join is blocked — no MTProto session/credentials,
//! UDP sendto EPERM even to loopback — so now-playing `voice` reports
//! `"opus-rtp-ready"` once the pipeline is proven, or
//! `"unavailable:<exact reason>"` otherwise; `joined:true` is never
//! reported without a real join.

mod common;
mod voice;

use common::*;
use std::collections::HashMap;
use std::io::Write;
use std::process::{Command, Stdio};
use std::sync::{Arc, LazyLock, Mutex};
use std::time::Duration;
use voice::RealVoiceCall;

const SERVICE: &str = "vc-rust";
const STATE_FILE: &str = "vc-state.json";
/// Duration for a track whose real duration is unknown: 0. The ticker
/// never auto-advances a 0-duration track (see `due_for_advance`) —
/// it plays until skipped or stopped. There is deliberately no fake
/// duration any more (A9): the old 180 s placeholder made the ticker
/// advance a track nobody had resolved, and hid resolve failures from
/// the core's engine fail-over.

// ---------------------------------------------------------------- types ---

#[derive(Clone, Debug)]
struct Track {
    id: String,
    title: String,
    artist: String,
    source: String,
    url: String,
    duration_sec: i64,
    /// "yt-dlp" | "url-pattern" | "pattern" — how this track was resolved.
    resolved_by: String,
    /// Local audio file downloaded by `download_audio` (mp3), if any.
    file_path: Option<String>,
    /// True once a local file exists. False + `download_failed` means the
    /// download was attempted and failed — the room still plays the track
    /// state-wise (old stub behaviour) instead of stalling forever.
    downloaded: bool,
    download_failed: bool,
}

#[derive(Clone, Debug)]
struct QueueItem {
    track: Track,
    requested_by: i64,
}

#[derive(Clone, Debug)]
struct Room {
    chat_id: i64,
    current: Option<Track>,
    queue: Vec<QueueItem>,
    paused: bool,
    position_sec: i64,
    volume: i64,
    /// ParvatiMusic `RoomState::loop_n`: repeats of the current track
    /// remaining (0 = off, 1..=10). Decremented on each replay.
    loop_n: i64,
    /// Shuffle mode: queue was shuffled on enable and `advance` picks a
    /// random queue index while on (ParvatiMusic `next_track`).
    shuffle: bool,
    /// Autoplay: when the queue empties, pick from the YouTube mix of
    /// the last track (ParvatiMusic `autoplay_tracks` / `mix_for`).
    autoplay: bool,
    /// Ids already played in this room (autoplay exclusion list).
    history: Vec<String>,
}

impl Room {
    fn new(chat_id: i64) -> Room {
        Room {
            chat_id,
            current: None,
            queue: Vec::new(),
            paused: false,
            position_sec: 0,
            volume: 100,
            loop_n: 0,
            shuffle: false,
            autoplay: false,
            history: Vec::new(),
        }
    }

    /// Record a track id in play history (autoplay exclusion), capped.
    fn note_played(&mut self, id: &str) {
        if !id.is_empty() && !self.history.iter().any(|h| h == id) {
            self.history.push(id.to_string());
            if self.history.len() > 200 {
                self.history.remove(0);
            }
        }
    }

    fn duration(&self) -> i64 {
        self.current.as_ref().map(|t| t.duration_sec).unwrap_or(0)
    }
}

#[derive(Debug, Default)]
struct State {
    rooms: HashMap<i64, Room>,
}

/// What `State::advance` decided for a room (ParvatiMusic
/// `RoomState::next_track` + `monitor::advance_locked`).
#[derive(Clone, Debug)]
enum AdvanceOutcome {
    /// Loop count left: replay the current track (same file, no download).
    Replay(Track),
    /// Next queued track (random index while shuffle is on).
    Next(Track),
    /// Queue empty and loop exhausted: room goes idle.
    Idle,
}

impl State {
    fn room_mut(&mut self, chat_id: i64) -> &mut Room {
        self.rooms.entry(chat_id).or_insert_with(|| Room::new(chat_id))
    }

    /// Advance to the next track (or replay / go idle). Returns the
    /// decision; the caller downloads `Next` tracks and starts them.
    fn advance(&mut self, chat_id: i64) -> AdvanceOutcome {
        let room = match self.rooms.get_mut(&chat_id) {
            Some(r) => r,
            None => return AdvanceOutcome::Idle,
        };
        // Loop first (ParvatiMusic room.rs next_track / monitor.rs
        // advance_locked): repeat the current track while loop_n > 0.
        if let Some(cur) = room.current.clone() {
            if room.loop_n > 0 {
                room.loop_n -= 1;
                room.position_sec = 0;
                room.paused = false;
                return AdvanceOutcome::Replay(cur);
            }
        }
        if room.queue.is_empty() {
            room.current = None;
            room.position_sec = 0;
            room.paused = false;
            return AdvanceOutcome::Idle;
        }
        let idx = if room.shuffle && room.queue.len() > 1 {
            random_below(room.queue.len())
        } else {
            0
        };
        let next = room.queue.remove(idx);
        room.note_played(&next.track.id);
        room.current = Some(next.track.clone());
        room.position_sec = 0;
        room.paused = false;
        AdvanceOutcome::Next(next.track)
    }

    /// Shuffle the queue in place (ParvatiMusic `RoomState::shuffle_queue`:
    /// Fisher–Yates) and enable shuffle mode.
    fn shuffle_queue(&mut self, chat_id: i64) {
        if let Some(room) = self.rooms.get_mut(&chat_id) {
            let n = room.queue.len();
            for i in (1..n).rev() {
                let j = random_below(i + 1);
                room.queue.swap(i, j);
            }
            room.shuffle = true;
        }
    }
}

/// xorshift64* from time nanos + pid — std-only randomness for shuffle
/// picks (no `rand` crate in this std-only service).
fn random_below(n: usize) -> usize {
    if n <= 1 {
        return 0;
    }
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos() as u64)
        .unwrap_or(0x9e3779b97f4a7c15);
    let mut x = now ^ ((std::process::id() as u64) << 32) | 1;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    (x % (n as u64)) as usize
}

// ------------------------------------------------------- JSON (de)code ---

fn track_json(t: &Track) -> Json {
    Json::Obj(vec![
        ("id".into(), Json::Str(t.id.clone())),
        ("title".into(), Json::Str(t.title.clone())),
        ("artist".into(), Json::Str(t.artist.clone())),
        ("source".into(), Json::Str(t.source.clone())),
        ("url".into(), Json::Str(t.url.clone())),
        ("duration_sec".into(), Json::Num(t.duration_sec as f64)),
        ("resolved_by".into(), Json::Str(t.resolved_by.clone())),
        (
            "file_path".into(),
            match &t.file_path {
                Some(p) => Json::Str(p.clone()),
                None => Json::Null,
            },
        ),
        ("downloaded".into(), Json::Bool(t.downloaded)),
        ("download_failed".into(), Json::Bool(t.download_failed)),
    ])
}

fn jstr(v: &Json, key: &str) -> Option<String> {
    v.get(key).and_then(|x| x.as_str()).map(|s| s.to_string())
}

fn ji64(v: &Json, key: &str) -> Option<i64> {
    v.get(key).and_then(|x| x.as_i64())
}

fn track_from_json(v: &Json) -> Option<Track> {
    let id = jstr(v, "id")?;
    let title = jstr(v, "title")?;
    Some(Track {
        id,
        title,
        artist: jstr(v, "artist").unwrap_or_default(),
        source: jstr(v, "source").unwrap_or_else(|| "youtube".to_string()),
        url: jstr(v, "url").unwrap_or_default(),
        // A persisted track without a duration has an UNKNOWN duration
        // (0) — never a fake one. The ticker never auto-advances it.
        duration_sec: ji64(v, "duration_sec").unwrap_or(0),
        resolved_by: jstr(v, "resolved_by").unwrap_or_else(|| "client".to_string()),
        file_path: jstr(v, "file_path"),
        downloaded: v.get("downloaded").and_then(|x| x.as_bool()).unwrap_or(false),
        download_failed: v
            .get("download_failed")
            .and_then(|x| x.as_bool())
            .unwrap_or(false),
    })
}

fn queue_json(queue: &[QueueItem]) -> Json {
    Json::Arr(
        queue
            .iter()
            .enumerate()
            .map(|(i, item)| {
                Json::Obj(vec![
                    ("position".into(), Json::Num(i as f64)),
                    ("requested_by".into(), Json::Num(item.requested_by as f64)),
                    ("track".into(), track_json(&item.track)),
                ])
            })
            .collect(),
    )
}

fn room_json(r: &Room) -> Json {
    Json::Obj(vec![
        ("chat_id".into(), Json::Num(r.chat_id as f64)),
        (
            "current".into(),
            match &r.current {
                Some(t) => track_json(t),
                None => Json::Null,
            },
        ),
        ("queue".into(), queue_json(&r.queue)),
        ("paused".into(), Json::Bool(r.paused)),
        ("position_sec".into(), Json::Num(r.position_sec as f64)),
        ("volume".into(), Json::Num(r.volume as f64)),
        ("loop".into(), Json::Num(r.loop_n as f64)),
        ("shuffle".into(), Json::Bool(r.shuffle)),
        ("autoplay".into(), Json::Bool(r.autoplay)),
        (
            "history".into(),
            Json::Arr(r.history.iter().map(|h| Json::Str(h.clone())).collect()),
        ),
    ])
}

fn room_from_json(v: &Json) -> Option<Room> {
    let chat_id = ji64(v, "chat_id")?;
    let current = match v.get("current") {
        Some(Json::Null) | None => None,
        Some(t) => track_from_json(t),
    };
    let mut queue = Vec::new();
    if let Some(Json::Arr(items)) = v.get("queue") {
        for it in items {
            let track = match it.get("track").and_then(track_from_json) {
                Some(t) => t,
                None => continue,
            };
            queue.push(QueueItem {
                track,
                requested_by: ji64(it, "requested_by").unwrap_or(0),
            });
        }
    }
    Some(Room {
        chat_id,
        current,
        queue,
        paused: v.get("paused").and_then(|x| x.as_bool()).unwrap_or(false),
        position_sec: ji64(v, "position_sec").unwrap_or(0),
        volume: ji64(v, "volume").unwrap_or(100),
        loop_n: ji64(v, "loop").or_else(|| ji64(v, "loop_n")).unwrap_or(0),
        shuffle: v.get("shuffle").and_then(|x| x.as_bool()).unwrap_or(false),
        autoplay: v
            .get("autoplay")
            .and_then(|x| x.as_bool())
            .unwrap_or(false),
        history: match v.get("history") {
            Some(Json::Arr(items)) => items
                .iter()
                .filter_map(|x| x.as_str().map(|s| s.to_string()))
                .collect(),
            _ => Vec::new(),
        },
    })
}

impl State {
    fn to_json(&self) -> Json {
        let mut rooms: Vec<&Room> = self.rooms.values().collect();
        rooms.sort_by_key(|r| r.chat_id);
        Json::Obj(vec![(
            "rooms".into(),
            Json::Arr(rooms.iter().map(|r| room_json(r)).collect()),
        )])
    }

    fn from_json(v: &Json) -> State {
        let mut st = State::default();
        if let Some(Json::Arr(items)) = v.get("rooms") {
            for it in items {
                if let Some(room) = room_from_json(it) {
                    st.rooms.insert(room.chat_id, room);
                }
            }
        }
        st
    }
}

fn save_state(st: &State) {
    let path = data_file(STATE_FILE);
    if let Err(e) = atomic_write(&path, &st.to_json().render()) {
        eprintln!("[vc-rust] failed to persist state to {}: {}", path.display(), e);
    }
}

fn load_state() -> State {
    let path = data_file(STATE_FILE);
    match std::fs::read_to_string(&path) {
        Ok(text) => match parse_json(&text) {
            Some(v) => {
                let st = State::from_json(&v);
                eprintln!(
                    "[vc-rust] restored {} room(s) from {}",
                    st.rooms.len(),
                    path.display()
                );
                st
            }
            None => {
                eprintln!("[vc-rust] WARNING: corrupt state file {}, starting empty", path.display());
                State::default()
            }
        },
        Err(_) => State::default(),
    }
}

// ------------------------------------------------------------- voice ---

/// Voice layer seam (ParvatiMusic: `mtproto/` + `ntg/` ntgcalls).
///
/// Joining a Telegram voice chat needs a live MTProto session plus the
/// ntgcalls WebRTC library. The real implementation is `RealVoiceCall`
/// (`voice.rs`): ffmpeg Opus/RTP pipeline + ntgcalls local offer; there
/// is no MTProto join client in-process (voice_bridge.py was deleted in
/// Phase 5 — join goes through the core-cpp TDLib backend). It reports the exact
/// failure when the backend is unavailable (no session/credentials,
/// UDP EPERM) and never claims `joined:true` without a real join.
/// Playback state still advances on the ticker; the queue flow works
/// with or without a live call.
trait VoiceCall: Send + Sync {
    fn join(&self, chat_id: i64);
    fn leave(&self, chat_id: i64);
    fn play_stream(&self, chat_id: i64, track: &Track);
    fn pause(&self, chat_id: i64);
    fn resume(&self, chat_id: i64);
    fn stop(&self, chat_id: i64);
}

static VOICE: LazyLock<RealVoiceCall> = LazyLock::new(RealVoiceCall::new);

// ------------------------------------------------------------ resolve ---

/// Run yt-dlp `-j` for one target (a watch URL or a `ytsearch1:`
/// query) and return the parsed info object, or None. Uses
/// `ytdlp_bin()` (PATH, then the downloaders venv) so resolve works
/// wherever the download path already works. Retries once with the
/// android player client when YouTube answers its bot check — the
/// same retry as `download_audio` and the downloaders service; no
/// cookies or credentials.
///
/// `-j` (not `--dump-single-json`): for a search it prints one JSON
/// object per line (JSONL), each a real video; `--dump-single-json`
/// on a search returns a playlist wrapper whose `id` is the query
/// string and which has no duration — the old code read that wrapper
/// and masked the missing duration with the fake 180. The first
/// object is taken either way.
fn ytdlp_info(target: &str) -> Option<Json> {
    let ytdlp = ytdlp_bin()?;
    let run = |extra: &[&str]| -> Option<std::process::Output> {
        let mut cmd = Command::new("timeout");
        cmd.arg("20")
            .arg(&ytdlp)
            .arg("-j")
            .arg("--no-playlist")
            .arg("--no-warnings");
        for e in extra {
            cmd.arg(e);
        }
        cmd.arg(target);
        cmd.output().ok()
    };
    let mut out = run(&[])?;
    if !out.status.success() && is_bot_check(&String::from_utf8_lossy(&out.stderr)) {
        if let Some(retry) = run(&["--extractor-args", "youtube:player_client=android"]) {
            out = retry;
        }
    }
    if !out.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&out.stdout);
    let text = text.trim();
    if let Some(v) = parse_json(text) {
        return Some(v);
    }
    parse_json(text.lines().next()?.trim())
}

/// Real metadata resolve for a YouTube watch URL (real title,
/// uploader, duration, stream URL — never a placeholder).
fn resolve_youtube_url(url: &str, id: &str) -> Option<Track> {
    let v = ytdlp_info(url)?;
    let title = v
        .get("title")
        .and_then(|x| x.as_str())
        .unwrap_or(id)
        .to_string();
    let artist = v
        .get("uploader")
        .and_then(|x| x.as_str())
        .unwrap_or("")
        .to_string();
    // No duration in the info means unknown (0), never a fake value;
    // the ticker never auto-advances such a track.
    let duration = v
        .get("duration")
        .and_then(|x| x.as_i64())
        .unwrap_or(0)
        .max(0);
    let page_url = v
        .get("webpage_url")
        .and_then(|x| x.as_str())
        .unwrap_or(url)
        .to_string();
    Some(Track {
        id: id.to_string(),
        title,
        artist,
        source: "youtube".to_string(),
        url: page_url,
        duration_sec: duration,
        resolved_by: "yt-dlp".to_string(),
        file_path: None,
        downloaded: false,
        download_failed: false,
    })
}

/// Real metadata resolve via yt-dlp for a free-text query
/// (`ytsearch1:<query>`); returns None when yt-dlp is missing, times
/// out, or errors — the caller then reports engine_unavailable.
fn resolve_via_ytdlp(query: &str) -> Option<Track> {
    let v = ytdlp_info(&format!("ytsearch1:{}", query))?;
    let id = v.get("id")?.as_str()?.to_string();
    let title = v
        .get("title")
        .and_then(|x| x.as_str())
        .unwrap_or(query)
        .to_string();
    let artist = v
        .get("uploader")
        .and_then(|x| x.as_str())
        .unwrap_or("")
        .to_string();
    let duration = v
        .get("duration")
        .and_then(|x| x.as_i64())
        .unwrap_or(0)
        .max(0);
    let url = v
        .get("webpage_url")
        .and_then(|x| x.as_str())
        .unwrap_or("")
        .to_string();
    Some(Track {
        id,
        title,
        artist,
        source: "youtube".to_string(),
        url,
        duration_sec: duration,
        resolved_by: "yt-dlp".to_string(),
        file_path: None,
        downloaded: false,
        download_failed: false,
    })
}

// ------------------------------------------------------- Spotify API ---
// Spotify track resolve via the Web API, client-credentials flow
// (env-gated, Phase 5 scope update): SPOTIFY_CLIENT_ID +
// SPOTIFY_CLIENT_SECRET are the only missing piece — without them the
// resolve fails honestly naming the vars; with them a Spotify URL
// resolves to real metadata (title / artists / duration_ms).
//
// std-only, so the API is called through the `curl` subprocess (like
// yt-dlp): credentials and the bearer token go on stdin via
// `--config -`, never on the argv, so the secret never shows in a
// process listing. The access token is cached in memory until
// expiry (same flow as vc-ts spotifySearchTracks).
//
// Sandbox note: the egress proxy rejects the token endpoint
// (accounts.spotify.com/api/token -> 502 "token refresh not allowed
// through egress proxy"), while api.spotify.com itself answers. The
// live path therefore needs the user's host (open egress) + the
// creds; from here only the honest-failure path is provable.

/// Spotify client credentials from the environment, or None.
fn spotify_creds() -> Option<(String, String)> {
    let id = std::env::var("SPOTIFY_CLIENT_ID").ok()?;
    let secret = std::env::var("SPOTIFY_CLIENT_SECRET").ok()?;
    if id.is_empty() || secret.is_empty() {
        return None;
    }
    Some((id, secret))
}

static SPOTIFY_TOKEN: LazyLock<Mutex<Option<(String, std::time::Instant)>>> =
    LazyLock::new(|| Mutex::new(None));

/// Escape a value for a curl --config string ("...").
fn curl_config_quote(s: &str) -> String {
    format!("\"{}\"", s.replace('\\', "\\\\").replace('"', "\\\""))
}

/// Run curl with a config fed on stdin and return the parsed JSON
/// body, or None on any failure (curl missing, non-zero exit,
/// unparsable output). `config_lines` are raw `key = value` lines.
fn curl_json(config_lines: &[String], url: &str) -> Option<Json> {
    let mut child = Command::new("curl")
        .arg("-s")
        .arg("-S")
        .arg("--max-time")
        .arg("15")
        .arg("--config")
        .arg("-")
        .arg(url)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .ok()?;
    let config = config_lines.join("\n") + "\n";
    if let Some(mut stdin) = child.stdin.take() {
        let _ = stdin.write_all(config.as_bytes());
    }
    let out = child.wait_with_output().ok()?;
    if !out.status.success() {
        return None;
    }
    parse_json(String::from_utf8_lossy(&out.stdout).trim())
}

/// Spotify client-credentials access token, cached until expiry.
fn spotify_client_token() -> Option<String> {
    {
        let guard = SPOTIFY_TOKEN.lock().unwrap_or_else(|e| e.into_inner());
        if let Some((token, expires_at)) = guard.as_ref() {
            if std::time::Instant::now() < *expires_at {
                return Some(token.clone());
            }
        }
    }
    let (id, secret) = spotify_creds()?;
    let v = curl_json(
        &[
            "request = \"POST\"".to_string(),
            "data = \"grant_type=client_credentials\"".to_string(),
            format!("user = {}", curl_config_quote(&format!("{}:{}", id, secret))),
        ],
        "https://accounts.spotify.com/api/token",
    )?;
    let token = v.get("access_token")?.as_str()?.to_string();
    let expires_in = v.get("expires_in").and_then(|x| x.as_i64()).unwrap_or(3600);
    let expires_at = std::time::Instant::now()
        + Duration::from_secs(expires_in.saturating_sub(60).max(0) as u64);
    let mut guard = SPOTIFY_TOKEN.lock().unwrap_or_else(|e| e.into_inner());
    *guard = Some((token.clone(), expires_at));
    Some(token)
}

/// Pure mapping: a Spotify track object -> Track. Separated out so
/// tests can exercise the mapping without network or credentials.
fn spotify_track_from_json(v: &Json, id: &str) -> Option<Track> {
    let title = v.get("name")?.as_str()?.to_string();
    let artists = match v.get("artists") {
        Some(Json::Arr(items)) => {
            let names: Vec<String> = items
                .iter()
                .filter_map(|a| a.get("name")?.as_str().map(|s| s.to_string()))
                .collect();
            names.join(", ")
        }
        _ => String::new(),
    };
    let duration = v
        .get("duration_ms")
        .and_then(|x| x.as_i64())
        .unwrap_or(0)
        .max(0)
        / 1000;
    let url = v
        .get("external_urls")
        .and_then(|u| u.get("spotify"))
        .and_then(|x| x.as_str())
        .unwrap_or("")
        .to_string();
    Some(Track {
        id: id.to_string(),
        title,
        artist: artists,
        source: "spotify".to_string(),
        url,
        duration_sec: duration,
        resolved_by: "spotify-api".to_string(),
        file_path: None,
        downloaded: false,
        download_failed: false,
    })
}

/// Resolve a Spotify track id via the Web API. Err when the creds
/// are missing (names them), when the token exchange fails, or when
/// the track lookup fails — all surfaced as engine_unavailable.
fn resolve_spotify_track(id: &str) -> Result<Track, String> {
    if spotify_creds().is_none() {
        return Err(
            "Spotify track cannot be resolved: SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET are not set (client-credentials flow)"
                .to_string(),
        );
    }
    let token = spotify_client_token().ok_or_else(|| {
        "Spotify token exchange failed (accounts.spotify.com/api/token unreachable or rejected)".to_string()
    })?;
    let url = format!("https://api.spotify.com/v1/tracks/{}", id);
    let v = curl_json(
        &[format!(
            "header = {}",
            curl_config_quote(&format!("Authorization: Bearer {}", token))
        )],
        &url,
    )
    .ok_or_else(|| format!("Spotify track lookup failed for {}", id))?;
    spotify_track_from_json(&v, id)
        .ok_or_else(|| format!("Spotify returned no usable track for {}", id))
}

/// Resolve a play query to a Track.
///
/// Order: YouTube URL/ID (real yt-dlp resolve of the watch URL) ->
/// Spotify URL (real Spotify Web API resolve when
/// SPOTIFY_CLIENT_ID/SECRET are set, honest error naming them when
/// not) -> yt-dlp subprocess (real metadata for a free-text query).
/// There is no pattern/placeholder fallback any more (A9): a query
/// yt-dlp cannot resolve is an Err, surfaced by the caller as
/// `engine_unavailable` (HTTP 502) so the core can fail over to
/// another engine — a fake track (invented title, fake 180 s
/// duration) would sit in the queue forever and hide the failure.
fn resolve_query(query: &str) -> Result<Track, String> {
    let q = query.trim();
    if let Some(id) = yt_id_from_url(q) {
        let url = format!("https://www.youtube.com/watch?v={}", id);
        return resolve_youtube_url(&url, &id)
            .ok_or_else(|| format!("yt-dlp resolve failed for YouTube URL {}", url));
    }
    if is_yt_id(q) {
        let url = format!("https://www.youtube.com/watch?v={}", q);
        return resolve_youtube_url(&url, q)
            .ok_or_else(|| format!("yt-dlp resolve failed for YouTube id {}", q));
    }
    if let Some(id) = spotify_id_from_url(q) {
        return resolve_spotify_track(&id);
    }
    resolve_via_ytdlp(q).ok_or_else(|| {
        format!(
            "yt-dlp resolve failed for query {:?} (yt-dlp missing, or YouTube unreachable/gated)",
            q
        )
    })
}

fn is_yt_id(s: &str) -> bool {
    s.len() == 11
        && s.chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_')
}

/// Extract a YouTube video id from a watch / youtu.be / shorts / embed /
/// live / v URL. Returns None for anything else.
fn yt_id_from_url(q: &str) -> Option<String> {
    let lower = q.to_ascii_lowercase();
    let take11 = |s: &str| -> Option<String> {
        let id: String = s
            .chars()
            .take_while(|c| c.is_ascii_alphanumeric() || *c == '-' || *c == '_')
            .take(11)
            .collect();
        if id.len() == 11 {
            Some(id)
        } else {
            None
        }
    };
    if let Some(p) = lower.find("youtu.be/") {
        return take11(&q[p + 9..]);
    }
    for marker in ["/shorts/", "/embed/", "/live/", "/v/"] {
        if let Some(p) = lower.find(marker) {
            return take11(&q[p + marker.len()..]);
        }
    }
    if lower.contains("youtube.com") || lower.contains("music.youtube.com") {
        if let Some(p) = lower.find("v=") {
            return take11(&q[p + 2..]);
        }
    }
    None
}

/// Extract a Spotify track id from an open.spotify.com URL or a
/// `spotify:track:<id>` URI. No API call — pure parsing.
fn spotify_id_from_url(q: &str) -> Option<String> {
    let lower = q.to_ascii_lowercase();
    let marker = "open.spotify.com/track/";
    if let Some(p) = lower.find(marker) {
        let rest = &q[p + marker.len()..];
        let id: String = rest
            .chars()
            .take_while(|c| c.is_ascii_alphanumeric())
            .collect();
        if !id.is_empty() {
            return Some(id);
        }
    }
    if let Some(rest) = lower.strip_prefix("spotify:track:") {
        let id: String = rest
            .chars()
            .take_while(|c| c.is_ascii_alphanumeric())
            .collect();
        if !id.is_empty() {
            return Some(id);
        }
    }
    None
}

// ----------------------------------------------------------- download ---

/// Locate the yt-dlp binary: $YT_DLP, else `yt-dlp` on PATH, else the
/// downloaders service venv (`services/downloaders/.venv/bin/yt-dlp`,
/// probed relative to the exe dir and the working directory).
/// Name unified with vc-ts (`$YT_DLP`, Phase 5 Worker E) — the old
/// $YTDLP_BIN is no longer read.
fn ytdlp_bin() -> Option<String> {
    if let Ok(p) = std::env::var("YT_DLP") {
        if !p.is_empty() && std::path::Path::new(&p).exists() {
            return Some(p);
        }
    }
    let probe = |p: &str| -> bool {
        Command::new(p)
            .arg("--version")
            .output()
            .map(|o| o.status.success())
            .unwrap_or(false)
    };
    if probe("yt-dlp") {
        return Some("yt-dlp".to_string());
    }
    let mut candidates: Vec<std::path::PathBuf> = Vec::new();
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            candidates.push(
                dir.join("../../downloaders/.venv/bin/yt-dlp"),
            );
            candidates.push(
                dir.join("../../../downloaders/.venv/bin/yt-dlp"),
            );
        }
    }
    candidates.push(std::path::PathBuf::from(
        "services/downloaders/.venv/bin/yt-dlp",
    ));
    candidates.push(std::path::PathBuf::from(
        "../downloaders/.venv/bin/yt-dlp",
    ));
    for c in candidates {
        if let Some(s) = c.to_str() {
            if probe(s) {
                return Some(s.to_string());
            }
        }
    }
    None
}

fn is_bot_check(stderr: &str) -> bool {
    stderr.contains("not a bot") || stderr.contains("Sign in to confirm")
}

/// Download a track's audio to a local mp3 file (ParvatiMusic downloads
/// the track before streaming it: `play.rs::RealSink::download_track`).
///
/// Target: the track URL when it is an http(s) URL, else the YouTube
/// watch URL for a youtube track, else a `ytsearch1:` query. Runs
/// `yt-dlp -f bestaudio --extract-audio --audio-format mp3` under a
/// 300 s `timeout`, retrying once with the android player client when
/// YouTube answers its bot check (same retry as the downloaders
/// service; no cookies or credentials). Returns the file path, or None
/// when yt-dlp is missing / fails — the caller marks the track
/// `download_failed` and keeps the room playing state-wise instead of
/// stalling.
fn download_audio(track: &Track, chat_id: i64) -> Option<String> {
    let ytdlp = ytdlp_bin()?;
    let target: String = if track.url.starts_with("http") {
        track.url.clone()
    } else if track.source == "youtube" && !track.id.is_empty() && !track.id.starts_with("pattern-") {
        format!("https://www.youtube.com/watch?v={}", track.id)
    } else {
        let q = format!("{} {}", track.artist, track.title).trim().to_string();
        if q.is_empty() {
            return None;
        }
        format!("ytsearch1:{}", q)
    };
    let base = data_file("downloads");
    let job_dir = base.join(format!("chat-{}", chat_id));
    if std::fs::create_dir_all(&job_dir).is_err() {
        return None;
    }
    let outtmpl = job_dir.join("%(id)s.%(ext)s");
    let outtmpl = outtmpl.to_string_lossy().to_string();
    let run = |extra: &[&str]| -> Option<std::process::Output> {
        let mut cmd = Command::new("timeout");
        cmd.arg("300").arg(&ytdlp);
        cmd.args([
            "--no-warnings",
            "--quiet",
            "--no-playlist",
            "--restrict-filenames",
            "--no-check-certificate",
            "--socket-timeout",
            "30",
            "--retries",
            "3",
            "-f",
            "bestaudio/best",
            "--extract-audio",
            "--audio-format",
            "mp3",
            "--audio-quality",
            "0",
            "-o",
            &outtmpl,
        ]);
        for e in extra {
            cmd.arg(e);
        }
        cmd.arg(&target);
        cmd.output().ok()
    };
    let mut out = run(&[])?;
    if !out.status.success() && is_bot_check(&String::from_utf8_lossy(&out.stderr)) {
        if let Some(retry) = run(&[
            "--extractor-args",
            "youtube:player_client=android",
        ]) {
            out = retry;
        }
    }
    if !out.status.success() {
        eprintln!(
            "[vc-rust] yt-dlp download failed for {}: {}",
            track.id,
            String::from_utf8_lossy(&out.stderr).trim()
        );
        return None;
    }
    // Newest mp3 in the job dir (this run's output).
    let mut files: Vec<_> = std::fs::read_dir(&job_dir)
        .ok()?
        .flatten()
        .filter(|e| {
            e.path()
                .extension()
                .map(|x| x == "mp3")
                .unwrap_or(false)
        })
        .collect();
    files.sort_by_key(|e| {
        e.metadata()
            .and_then(|m| m.modified())
            .unwrap_or(std::time::SystemTime::UNIX_EPOCH)
    });
    files
        .pop()
        .map(|e| e.path().to_string_lossy().to_string())
}

/// Autoplay pick (ParvatiMusic `platforms/youtube.rs::mix_for` +
/// `registry::autoplay_tracks` + `golden_zone_pick`): fetch the YouTube
/// mix playlist `RD<video_id>` flat, take the first entry whose id is
/// neither the last track nor in `history`. Returns None when yt-dlp is
/// missing, the mix is empty, or every candidate was already played.
fn autoplay_pick(last: &Track, history: &[String]) -> Option<Track> {
    let ytdlp = ytdlp_bin()?;
    if last.source != "youtube" || last.id.is_empty() || last.id.starts_with("pattern-") {
        return None;
    }
    let mix_url = format!(
        "https://www.youtube.com/watch?v={}&list=RD{}",
        last.id, last.id
    );
    // NOTE: no --no-playlist here — the mix IS a playlist (RD<id>);
    // with --no-playlist yt-dlp returns only the single video, and the
    // bare playlist URL errors "This playlist type is unviewable".
    // The watch URL with list=RD<id> returns the mix flat (360 entries).
    let run_mix = |extra: &[&str]| -> Option<std::process::Output> {
        let mut cmd = Command::new("timeout");
        cmd.arg("60").arg(&ytdlp);
        cmd.args(["--flat-playlist", "-J", "--no-warnings", "--quiet"]);
        for e in extra {
            cmd.arg(e);
        }
        cmd.arg(&mix_url);
        cmd.output().ok()
    };
    let mut out = run_mix(&[])?;
    // Same bot-check retry as the download path: the mix endpoint is
    // sometimes gated for the default (web) player client from this
    // egress IP; the android client is not gated the same way.
    if !out.status.success() && is_bot_check(&String::from_utf8_lossy(&out.stderr)) {
        if let Some(retry) = run_mix(&[
            "--extractor-args",
            "youtube:player_client=android",
        ]) {
            out = retry;
        }
    }
    if !out.status.success() {
        eprintln!(
            "[vc-rust] autoplay mix fetch failed for {}: {}",
            last.id,
            String::from_utf8_lossy(&out.stderr).trim()
        );
        return None;
    }
    let text = String::from_utf8_lossy(&out.stdout);
    let v = match parse_json(text.trim()) {
        Some(v) => v,
        None => {
            eprintln!(
                "[vc-rust] autoplay mix JSON unparsable for {} ({} bytes stdout)",
                last.id,
                out.stdout.len()
            );
            return None;
        }
    };
    let entries = match &v {
        Json::Arr(items) => items.clone(),
        Json::Obj(_) => match v.get("entries") {
            Some(Json::Arr(items)) => items.clone(),
            _ => return None,
        },
        _ => return None,
    };
    first_unplayed_entry(&entries, &last.id, history)
}

/// Pure part of the autoplay pick: first flat-playlist entry whose id is
/// neither the last track nor in `history`. Separated out so tests can
/// exercise the exclusion rule without network.
fn first_unplayed_entry(
    entries: &[Json],
    last_id: &str,
    history: &[String],
) -> Option<Track> {
    for e in entries {
        let id = match e.get("id").and_then(|x| x.as_str()) {
            Some(id) => id.to_string(),
            None => continue,
        };
        if id == last_id || history.iter().any(|h| h == &id) {
            continue;
        }
        let title = e
            .get("title")
            .and_then(|x| x.as_str())
            .unwrap_or(&id)
            .to_string();
        let artist = e
            .get("uploader")
            .and_then(|x| x.as_str())
            .unwrap_or("")
            .to_string();
        let duration = e
            .get("duration")
            .and_then(|x| x.as_i64())
            .unwrap_or(0)
            .max(0);
        return Some(Track {
            id: id.clone(),
            title,
            artist,
            source: "youtube".to_string(),
            url: format!("https://www.youtube.com/watch?v={}", id),
            duration_sec: duration,
            resolved_by: "autoplay-mix".to_string(),
            file_path: None,
            downloaded: false,
            download_failed: false,
        });
    }
    None
}

/// Start `track` as the room's current track: download its audio
/// (best effort), record it in history, reset position, and hand it to
/// the voice layer. Returns the track with download fields filled in.
fn start_track(room: &mut Room, mut track: Track, chat_id: i64) -> Track {
    if let Some(path) = download_audio(&track, chat_id) {
        track.file_path = Some(path);
        track.downloaded = true;
        track.download_failed = false;
    } else {
        track.downloaded = false;
        track.download_failed = true;
        eprintln!(
            "[vc-rust] no local audio for {} in chat {} (state keeps playing)",
            track.id, chat_id
        );
    }
    room.note_played(&track.id);
    room.current = Some(track.clone());
    room.position_sec = 0;
    room.paused = false;
    VOICE.play_stream(chat_id, &track);
    track
}

// ------------------------------------------------------------ helpers ---

fn lock(state: &Arc<Mutex<State>>) -> std::sync::MutexGuard<'_, State> {
    state.lock().unwrap_or_else(|e| e.into_inner())
}

fn now_playing_json(room: &Room) -> Json {
    Json::Obj(vec![
        ("chat_id".into(), Json::Num(room.chat_id as f64)),
        ("engine".into(), Json::Str("rust".to_string())),
        ("paused".into(), Json::Bool(room.paused)),
        ("position_sec".into(), Json::Num(room.position_sec as f64)),
        ("volume".into(), Json::Num(room.volume as f64)),
        (
            "track".into(),
            match &room.current {
                Some(t) => track_json(t),
                None => Json::Null,
            },
        ),
        ("queue".into(), queue_json(&room.queue)),
        ("voice".into(), Json::Str(VOICE.voice_string(room.chat_id))),
        ("loop".into(), Json::Num(room.loop_n as f64)),
        ("shuffle".into(), Json::Bool(room.shuffle)),
        ("autoplay".into(), Json::Bool(room.autoplay)),
    ])
}

fn need_chat(body: &Json) -> Result<i64, (u16, String)> {
    body.get("chat_id")
        .and_then(|v| v.as_i64())
        .ok_or((400, err("bad_request", "missing integer field: chat_id")))
}

// ------------------------------------------------------------ handlers ---

/// /play: like ParvatiMusic's handle_play — if the room is idle the
/// track starts at once (downloaded first, `start_track`); if
/// something is already playing it is queued (ParvatiMusic queues
/// rather than interrupting). `track` is resolved by the caller
/// *outside* the state lock: `resolve_query` can run yt-dlp for
/// seconds and must not block other chats.
fn handle_play(st: &mut State, body: &Json, track: Track) -> (u16, String) {
    let chat_id = match need_chat(body) {
        Ok(id) => id,
        Err(e) => return e,
    };
    let requested_by = body.get("requested_by").and_then(|v| v.as_i64()).unwrap_or(0);
    let resolved_by = track.resolved_by.clone();
    let room = st.room_mut(chat_id);
    let queued;
    if room.current.is_none() {
        VOICE.join(chat_id);
        start_track(room, track, chat_id);
        queued = false;
    } else {
        room.queue.push(QueueItem {
            track: track.clone(),
            requested_by,
        });
        queued = true;
    }
    let np = now_playing_json(room);
    (
        200,
        ok(&format!(
            "{{\"now_playing\":{},\"queued\":{},\"resolved_by\":{}}}",
            np.render(),
            queued,
            quote(&resolved_by)
        )),
    )
}

fn handle_pause(st: &mut State, body: &Json) -> (u16, String) {
    let chat_id = match need_chat(body) {
        Ok(id) => id,
        Err(e) => return e,
    };
    let room = match st.rooms.get_mut(&chat_id) {
        Some(r) if r.current.is_some() => r,
        _ => return (400, err("bad_request", "nothing playing in this chat")),
    };
    room.paused = true;
    VOICE.pause(chat_id);
    let np = now_playing_json(room);
    (200, ok(&format!("{{\"now_playing\":{}}}", np.render())))
}

fn handle_resume(st: &mut State, body: &Json) -> (u16, String) {
    let chat_id = match need_chat(body) {
        Ok(id) => id,
        Err(e) => return e,
    };
    let room = match st.rooms.get_mut(&chat_id) {
        Some(r) if r.current.is_some() => r,
        _ => return (400, err("bad_request", "nothing playing in this chat")),
    };
    room.paused = false;
    VOICE.resume(chat_id);
    let np = now_playing_json(room);
    (200, ok(&format!("{{\"now_playing\":{}}}", np.render())))
}

fn handle_skip(st: &mut State, body: &Json) -> (u16, String) {
    let chat_id = match need_chat(body) {
        Ok(id) => id,
        Err(e) => return e,
    };
    {
        let has_something = match st.rooms.get(&chat_id) {
            Some(r) => r.current.is_some() || !r.queue.is_empty(),
            None => false,
        };
        if !has_something {
            return (400, err("bad_request", "nothing playing in this chat"));
        }
    }
    match st.advance(chat_id) {
        AdvanceOutcome::Replay(t) => VOICE.play_stream(chat_id, &t),
        AdvanceOutcome::Next(t) => {
            // The next track starts now, so its audio is downloaded
            // first (same as /play start); a failed download leaves the
            // room playing state-wise, it never blocks the skip.
            let (path, ok) = match download_audio(&t, chat_id) {
                Some(p) => (Some(p), true),
                None => (None, false),
            };
            {
                let room = st.room_mut(chat_id);
                if let Some(cur) = room.current.as_mut() {
                    if cur.id == t.id {
                        cur.file_path = path;
                        cur.downloaded = ok;
                        cur.download_failed = !ok;
                    }
                }
            }
            VOICE.play_stream(chat_id, &t);
        }
        AdvanceOutcome::Idle => VOICE.stop(chat_id),
    }
    let room = st.room_mut(chat_id);
    let np = now_playing_json(room);
    (200, ok(&format!("{{\"now_playing\":{}}}", np.render())))
}

fn handle_stop(st: &mut State, body: &Json) -> (u16, String) {
    let chat_id = match need_chat(body) {
        Ok(id) => id,
        Err(e) => return e,
    };
    let room = st.room_mut(chat_id);
    room.current = None;
    room.queue.clear();
    room.position_sec = 0;
    room.paused = false;
    VOICE.stop(chat_id);
    VOICE.leave(chat_id);
    (200, ok("{}"))
}

fn handle_seek(st: &mut State, body: &Json) -> (u16, String) {
    let chat_id = match need_chat(body) {
        Ok(id) => id,
        Err(e) => return e,
    };
    let position = match body.get("position_sec").and_then(|v| v.as_i64()) {
        Some(p) => p,
        None => {
            return (
                400,
                err("bad_request", "missing integer field: position_sec"),
            )
        }
    };
    let room = match st.rooms.get_mut(&chat_id) {
        Some(r) if r.current.is_some() => r,
        _ => return (400, err("bad_request", "nothing playing in this chat")),
    };
    let max = room.duration();
    room.position_sec = if max > 0 {
        position.clamp(0, max)
    } else {
        position.max(0)
    };
    let np = now_playing_json(room);
    (200, ok(&format!("{{\"now_playing\":{}}}", np.render())))
}

fn handle_queue(st: &mut State, body: &Json) -> (u16, String) {
    let chat_id = match need_chat(body) {
        Ok(id) => id,
        Err(e) => return e,
    };
    let action = match body.get("action").and_then(|v| v.as_str()) {
        Some(a) => a.to_string(),
        None => return (400, err("bad_request", "missing string field: action")),
    };
    let room = st.room_mut(chat_id);
    match action.as_str() {
        "add" => {
            let track = match body.get("track").and_then(track_from_json) {
                Some(t) => t,
                None => {
                    return (
                        400,
                        err("bad_request", "missing/invalid object field: track (needs id, title)"),
                    )
                }
            };
            let requested_by = body.get("requested_by").and_then(|v| v.as_i64()).unwrap_or(0);
            room.queue.push(QueueItem { track, requested_by });
        }
        "remove" => {
            let pos = match body.get("position").and_then(|v| v.as_i64()) {
                Some(p) => p,
                None => {
                    return (
                        400,
                        err("bad_request", "missing integer field: position (for remove)"),
                    )
                }
            };
            if pos < 0 || pos as usize >= room.queue.len() {
                return (
                    400,
                    err(
                        "bad_request",
                        &format!("position {} out of range (queue len {})", pos, room.queue.len()),
                    ),
                );
            }
            room.queue.remove(pos as usize);
        }
        "clear" => room.queue.clear(),
        "list" => {}
        _ => {
            return (
                400,
                err("bad_request", "unknown action (add|remove|clear|list)"),
            )
        }
    }
    let q = queue_json(&room.queue);
    (200, ok(&format!("{{\"queue\":{}}}", q.render())))
}

/// /loop: ParvatiMusic `vc.rs::handle_loop` — `/loop <0..=10>` sets the
/// repeat count for the current track (0 = off). The count is
/// decremented on each replay by `State::advance`.
fn handle_loop(st: &mut State, body: &Json) -> (u16, String) {
    let chat_id = match need_chat(body) {
        Ok(id) => id,
        Err(e) => return e,
    };
    let count = match body.get("count").and_then(|v| v.as_i64()) {
        Some(c) => c,
        None => {
            return (
                400,
                err("bad_request", "missing integer field: count (0..=10)"),
            )
        }
    };
    if !(0..=10).contains(&count) {
        return (400, err("bad_request", "count must be 0..=10"));
    }
    let room = st.room_mut(chat_id);
    room.loop_n = count;
    let np = now_playing_json(room);
    (200, ok(&format!("{{\"now_playing\":{}}}", np.render())))
}

/// /shuffle: ParvatiMusic `vc.rs::handle_shuffle` — enabling shuffles
/// the queue immediately and makes `advance` pick a random index;
/// disabling keeps the current order.
fn handle_shuffle(st: &mut State, body: &Json) -> (u16, String) {
    let chat_id = match need_chat(body) {
        Ok(id) => id,
        Err(e) => return e,
    };
    let enabled = match body.get("enabled").and_then(|v| v.as_bool()) {
        Some(e) => e,
        None => {
            return (
                400,
                err("bad_request", "missing boolean field: enabled"),
            )
        }
    };
    if enabled {
        st.shuffle_queue(chat_id);
    } else if let Some(room) = st.rooms.get_mut(&chat_id) {
        room.shuffle = false;
    }
    let room = st.room_mut(chat_id);
    let np = now_playing_json(room);
    (200, ok(&format!("{{\"now_playing\":{}}}", np.render())))
}

/// /autoplay: ParvatiMusic `lists.rs::autoplay_handler` — when on, the
/// ticker picks from the last track's YouTube mix once the queue
/// empties (see `autoplay_pick`).
fn handle_autoplay(st: &mut State, body: &Json) -> (u16, String) {
    let chat_id = match need_chat(body) {
        Ok(id) => id,
        Err(e) => return e,
    };
    let enabled = match body.get("enabled").and_then(|v| v.as_bool()) {
        Some(e) => e,
        None => {
            return (
                400,
                err("bad_request", "missing boolean field: enabled"),
            )
        }
    };
    let room = st.room_mut(chat_id);
    room.autoplay = enabled;
    let np = now_playing_json(room);
    (200, ok(&format!("{{\"now_playing\":{}}}", np.render())))
}

/// /autoplay-skip: ParvatiMusic `vc.rs::handle_autoplay_skip` (`/askip`)
/// — only when autoplay is on: pick a mix track from the current track
/// now, reset the loop count, and force-play it.
fn handle_autoplay_skip(st: &mut State, body: &Json) -> (u16, String) {
    let chat_id = match need_chat(body) {
        Ok(id) => id,
        Err(e) => return e,
    };
    let (last, history) = match st.rooms.get(&chat_id) {
        Some(r) if r.autoplay => match &r.current {
            Some(t) => (t.clone(), r.history.clone()),
            None => {
                return (400, err("bad_request", "nothing playing in this chat"))
            }
        },
        Some(_) => {
            return (
                400,
                err("bad_request", "autoplay is off in this chat"),
            )
        }
        None => {
            return (400, err("bad_request", "nothing playing in this chat"))
        }
    };
    let pick = match autoplay_pick(&last, &history) {
        Some(p) => p,
        None => {
            return (
                502,
                err(
                    "download_failed",
                    "no autoplay recommendation for the current track",
                ),
            )
        }
    };
    let room = st.room_mut(chat_id);
    room.loop_n = 0;
    start_track(room, pick, chat_id);
    let np = now_playing_json(room);
    (200, ok(&format!("{{\"now_playing\":{}}}", np.render())))
}

fn handle(state: &Arc<Mutex<State>>, req: &Request) -> (u16, String) {
    if req.method == "GET" && req.path == "/v1/now-playing" {
        let chat_id = match req
            .query_param("chat_id")
            .and_then(|v| v.parse::<i64>().ok())
        {
            Some(id) => id,
            None => {
                return (
                    400,
                    err("bad_request", "missing integer query param: chat_id"),
                )
            }
        };
        let st = lock(state);
        let np = match st.rooms.get(&chat_id) {
            Some(room) => now_playing_json(room),
            None => now_playing_json(&Room::new(chat_id)),
        };
        return (200, ok(&format!("{{\"now_playing\":{}}}", np.render())));
    }
    if req.method != "POST" {
        return (404, err("not_found", "route not found"));
    }
    let body = match parse_json(&req.body) {
        Some(b @ Json::Obj(_)) => b,
        _ => return (400, err("bad_request", "invalid JSON body")),
    };
    // /play resolves outside the state lock: resolve_query can run
    // yt-dlp for seconds and must not block other chats. A failed
    // resolve is engine_unavailable (HTTP 502) — no placeholder track
    // is ever queued (A9).
    let pre_track = if req.path == "/v1/play" {
        match body.get("query").and_then(|v| v.as_str()) {
            Some(q) if !q.trim().is_empty() => match resolve_query(q) {
                Ok(t) => Some(t),
                Err(e) => return (502, err("engine_unavailable", &e)),
            },
            _ => None,
        }
    } else {
        None
    };
    let mut st = lock(state);
    let res = match req.path.as_str() {
        "/v1/play" => match pre_track {
            Some(t) => handle_play(&mut st, &body, t),
            None => (400, err("bad_request", "missing string field: query")),
        },
        "/v1/pause" => handle_pause(&mut st, &body),
        "/v1/resume" => handle_resume(&mut st, &body),
        "/v1/skip" => handle_skip(&mut st, &body),
        "/v1/stop" => handle_stop(&mut st, &body),
        "/v1/seek" => handle_seek(&mut st, &body),
        "/v1/queue" => handle_queue(&mut st, &body),
        "/v1/loop" => handle_loop(&mut st, &body),
        "/v1/shuffle" => handle_shuffle(&mut st, &body),
        "/v1/autoplay" => handle_autoplay(&mut st, &body),
        "/v1/autoplay-skip" => handle_autoplay_skip(&mut st, &body),
        _ => return (404, err("not_found", "route not found")),
    };
    save_state(&st);
    res
}

// ------------------------------------------------------------- ticker ---

/// 1s ticker: advance playback position; when a track's duration
/// elapses, auto-advance to the next queued track (ParvatiMusic's room
/// advances on track end the same way). Position freezes while a track's
/// audio is still downloading (`downloaded` and `download_failed` both
/// false) so a slow download cannot skip the track. Loop replays the
/// current track; when the queue empties and autoplay is on, a mix pick
/// is fetched from the last track (ParvatiMusic `monitor::advance_locked`).
/// Downloads happen outside the state lock; state is persisted only on
/// a track change, not every second.
/// Pure advance decision, extracted for tests: a room only advances
/// when its audio is ready (downloaded, or its download failed and
/// the room keeps playing state-wise), it is not paused, and its
/// track has a KNOWN duration (duration_sec > 0) that the position
/// has reached. Unknown-duration tracks (duration_sec 0 — e.g. an
/// autoplay mix entry without a duration, or a client-supplied track
/// without one) never auto-advance: they play until skipped or
/// stopped. (A9: no fake durations, and the ticker must not advance
/// on an unknown duration.)
fn due_for_advance(room: &Room) -> bool {
    match &room.current {
        Some(t) if t.downloaded || t.download_failed => {}
        _ => return false,
    }
    !room.paused && room.duration() > 0 && room.position_sec >= room.duration()
}

fn ticker(state: Arc<Mutex<State>>) {
    loop {
        std::thread::sleep(Duration::from_secs(1));
        let mut to_replay: Vec<(i64, Track)> = Vec::new();
        let mut to_next: Vec<(i64, Track)> = Vec::new();
        let mut to_stop: Vec<i64> = Vec::new();
        let mut autoplay_pending: Vec<(i64, Track, Vec<String>)> = Vec::new();
        let mut changed = false;
        {
            let mut st = lock(&state);
            let mut advancing: Vec<i64> = Vec::new();
            for (chat_id, room) in st.rooms.iter_mut() {
                let ready = match &room.current {
                    // Only advance position once the audio file is
                    // present or its download has failed (state keeps
                    // playing in that case); never while downloading.
                    Some(t) => t.downloaded || t.download_failed,
                    None => false,
                };
                if ready && !room.paused && room.duration() > 0 {
                    room.position_sec += 1;
                }
                if due_for_advance(room) {
                    advancing.push(*chat_id);
                }
            }
            for chat_id in advancing {
                let last = st
                    .rooms
                    .get(&chat_id)
                    .and_then(|r| r.current.clone());
                let history = st
                    .rooms
                    .get(&chat_id)
                    .map(|r| r.history.clone())
                    .unwrap_or_default();
                let autoplay_on = st
                    .rooms
                    .get(&chat_id)
                    .map(|r| r.autoplay)
                    .unwrap_or(false);
                match st.advance(chat_id) {
                    AdvanceOutcome::Replay(t) => to_replay.push((chat_id, t)),
                    AdvanceOutcome::Next(t) => to_next.push((chat_id, t)),
                    AdvanceOutcome::Idle => {
                        if autoplay_on {
                            match last {
                                Some(l) => {
                                    autoplay_pending.push((chat_id, l, history))
                                }
                                None => to_stop.push(chat_id),
                            }
                        } else {
                            to_stop.push(chat_id);
                        }
                    }
                }
                changed = true;
            }
            if changed {
                save_state(&st);
            }
        }
        // Downloads + mix picks outside the lock.
        let mut downloaded_next: Vec<(i64, Track)> = Vec::new();
        for (chat_id, track) in to_next {
            let (path, ok) = match download_audio(&track, chat_id) {
                Some(p) => (Some(p), true),
                None => (None, false),
            };
            {
                let mut st = lock(&state);
                if let Some(room) = st.rooms.get_mut(&chat_id) {
                    if let Some(cur) = room.current.as_mut() {
                        if cur.id == track.id {
                            cur.file_path = path;
                            cur.downloaded = ok;
                            cur.download_failed = !ok;
                        }
                    }
                }
                save_state(&st);
            }
            downloaded_next.push((chat_id, track));
        }
        let mut autoplay_started: Vec<(i64, Track)> = Vec::new();
        for (chat_id, last, history) in autoplay_pending {
            if let Some(pick) = autoplay_pick(&last, &history) {
                let mut track = pick;
                if let Some(path) = download_audio(&track, chat_id) {
                    track.file_path = Some(path);
                    track.downloaded = true;
                } else {
                    track.download_failed = true;
                }
                {
                    let mut st = lock(&state);
                    // Only start if the room is still idle — a /play
                    // may have claimed it while we were downloading.
                    if let Some(room) = st.rooms.get_mut(&chat_id) {
                        if room.current.is_none() {
                            room.note_played(&track.id);
                            room.current = Some(track.clone());
                            room.position_sec = 0;
                            room.paused = false;
                            autoplay_started.push((chat_id, track));
                        }
                    }
                    save_state(&st);
                }
            } else {
                to_stop.push(chat_id);
            }
        }
        for (chat_id, track) in to_replay {
            VOICE.play_stream(chat_id, &track);
        }
        for (chat_id, track) in downloaded_next {
            VOICE.play_stream(chat_id, &track);
        }
        for (chat_id, track) in autoplay_started {
            VOICE.play_stream(chat_id, &track);
        }
        for chat_id in to_stop {
            VOICE.stop(chat_id);
        }
    }
}

fn main() {
    let port: u16 = std::env::var("PORT")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(8003);
    let state = Arc::new(Mutex::new(load_state()));
    let tick_state = Arc::clone(&state);
    std::thread::spawn(move || ticker(tick_state));
    let handler_state = Arc::clone(&state);
    let handler = move |req: &Request| handle(&handler_state, req);
    if let Err(e) = run_server(port, SERVICE, handler) {
        eprintln!("parlay {} failed to listen on {}: {}", SERVICE, port, e);
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn mk_track(id: &str) -> Track {
        Track {
            id: id.to_string(),
            title: format!("Track {}", id),
            artist: "Test".to_string(),
            source: "youtube".to_string(),
            url: format!("https://www.youtube.com/watch?v={}", id),
            duration_sec: 180,
            resolved_by: "test".to_string(),
            file_path: None,
            downloaded: true,
            download_failed: false,
        }
    }

    fn mk_room_with_queue(ids: &[&str]) -> (State, i64) {
        let mut st = State::default();
        let chat_id = 42;
        let room = st.room_mut(chat_id);
        room.current = Some(mk_track("current"));
        for id in ids {
            room.queue.push(QueueItem {
                track: mk_track(id),
                requested_by: 1,
            });
        }
        (st, chat_id)
    }

    #[test]
    fn loop_replays_then_advances() {
        let (mut st, chat_id) = mk_room_with_queue(&["a", "b"]);
        st.rooms.get_mut(&chat_id).unwrap().loop_n = 2;
        // Two replays of the current track, loop_n draining 2 -> 0.
        match st.advance(chat_id) {
            AdvanceOutcome::Replay(t) => assert_eq!(t.id, "current"),
            other => panic!("expected Replay, got {:?}", other),
        }
        assert_eq!(st.rooms[&chat_id].loop_n, 1);
        match st.advance(chat_id) {
            AdvanceOutcome::Replay(t) => assert_eq!(t.id, "current"),
            other => panic!("expected Replay, got {:?}", other),
        }
        assert_eq!(st.rooms[&chat_id].loop_n, 0);
        // Loop exhausted: next queued track.
        match st.advance(chat_id) {
            AdvanceOutcome::Next(t) => assert_eq!(t.id, "a"),
            other => panic!("expected Next(a), got {:?}", other),
        }
        match st.advance(chat_id) {
            AdvanceOutcome::Next(t) => assert_eq!(t.id, "b"),
            other => panic!("expected Next(b), got {:?}", other),
        }
        match st.advance(chat_id) {
            AdvanceOutcome::Idle => {}
            other => panic!("expected Idle, got {:?}", other),
        }
        assert!(st.rooms[&chat_id].current.is_none());
    }

    #[test]
    fn loop_zero_means_off() {
        let (mut st, chat_id) = mk_room_with_queue(&["a"]);
        match st.advance(chat_id) {
            AdvanceOutcome::Next(t) => assert_eq!(t.id, "a"),
            other => panic!("expected Next(a), got {:?}", other),
        }
    }

    #[test]
    fn shuffle_advance_plays_everything_once() {
        let (mut st, chat_id) = mk_room_with_queue(&["a", "b", "c", "d", "e"]);
        st.rooms.get_mut(&chat_id).unwrap().shuffle = true;
        let mut seen = Vec::new();
        loop {
            match st.advance(chat_id) {
                AdvanceOutcome::Next(t) => seen.push(t.id),
                AdvanceOutcome::Idle => break,
                AdvanceOutcome::Replay(t) => panic!("unexpected replay of {}", t.id),
            }
        }
        seen.sort();
        assert_eq!(seen, vec!["a", "b", "c", "d", "e"]);
    }

    #[test]
    fn shuffle_queue_preserves_items_and_enables_mode() {
        let (mut st, chat_id) = mk_room_with_queue(&["a", "b", "c", "d", "e", "f"]);
        st.shuffle_queue(chat_id);
        let room = &st.rooms[&chat_id];
        assert!(room.shuffle);
        let mut ids: Vec<&str> = room.queue.iter().map(|q| q.track.id.as_str()).collect();
        ids.sort();
        assert_eq!(ids, vec!["a", "b", "c", "d", "e", "f"]);
    }

    #[test]
    fn autoplay_pick_skips_last_and_history() {
        let entries = vec![
            Json::Obj(vec![
                ("id".into(), Json::Str("played1".into())),
                ("title".into(), Json::Str("Played One".into())),
            ]),
            Json::Obj(vec![
                ("id".into(), Json::Str("last-id".into())),
                ("title".into(), Json::Str("Last".into())),
            ]),
            Json::Obj(vec![
                ("id".into(), Json::Str("fresh1".into())),
                ("title".into(), Json::Str("Fresh One".into())),
                ("uploader".into(), Json::Str("Some Channel".into())),
                ("duration".into(), Json::Num(200.0)),
            ]),
        ];
        let history = vec!["played1".to_string()];
        let pick = first_unplayed_entry(&entries, "last-id", &history).expect("a pick");
        assert_eq!(pick.id, "fresh1");
        assert_eq!(pick.title, "Fresh One");
        assert_eq!(pick.artist, "Some Channel");
        assert_eq!(pick.duration_sec, 200);
        assert_eq!(pick.resolved_by, "autoplay-mix");
        // Everything already played -> None.
        let all_played = vec!["played1".to_string(), "fresh1".to_string()];
        assert!(first_unplayed_entry(&entries, "last-id", &all_played).is_none());
    }

    #[test]
    fn history_records_plays_and_caps() {
        let (mut st, chat_id) = mk_room_with_queue(&[]);
        let room = st.room_mut(chat_id);
        room.note_played("x");
        room.note_played("x"); // no duplicate
        room.note_played("y");
        assert_eq!(room.history, vec!["x".to_string(), "y".to_string()]);
    }

    #[test]
    fn ytdlp_binary_is_findable() {
        // The download path is dead without yt-dlp; the sandbox keeps it
        // in the downloaders venv, which ytdlp_bin() must discover.
        assert!(ytdlp_bin().is_some(), "no yt-dlp binary found");
    }

    /// Live download test: downloads a real 3:33 track as mp3. Gated on
    /// PARLAY_LIVE_TEST=1 (network + ~20 s are not for every
    /// `cargo test` run).
    #[test]
    fn live_download_short_video() {
        if std::env::var("PARLAY_LIVE_TEST").is_err() {
            eprintln!("skipping live download (set PARLAY_LIVE_TEST=1)");
            return;
        }
        let track = mk_track("dQw4w9WgXcQ");
        let path = download_audio(&track, 424242).expect("download produced a file");
        let meta = std::fs::metadata(&path).expect("downloaded file exists");
        assert!(meta.len() > 10_000, "mp3 suspiciously small: {} bytes", meta.len());
        assert!(path.ends_with(".mp3"), "expected an mp3, got {}", path);
        eprintln!("live download OK: {} ({} bytes)", path, meta.len());
    }

    /// Live autoplay test: the mix playlist for the test video must
    /// yield at least one unplayed candidate. Gated like the download.
    #[test]
    fn live_autoplay_mix_pick() {
        if std::env::var("PARLAY_LIVE_TEST").is_err() {
            eprintln!("skipping live autoplay (set PARLAY_LIVE_TEST=1)");
            return;
        }
        let last = mk_track("dQw4w9WgXcQ");
        let pick = autoplay_pick(&last, &[]);
        assert!(pick.is_some(), "mix playlist returned no candidate");
        let pick = pick.unwrap();
        assert_ne!(pick.id, last.id);
        eprintln!("live autoplay pick: {} — {}", pick.id, pick.title);
    }

    // ---- Phase 5 (A9): no placeholder tracks, no fake durations ----

    /// A YouTube URL resolves to real metadata (title, real duration)
    /// or fails honestly — never a fake "YouTube video <id>" track.
    /// Tolerant of YouTube gating the sandbox egress IP at test time:
    /// then the error must name yt-dlp.
    #[test]
    fn youtube_url_resolve_is_real_or_honest_error() {
        match resolve_query("https://www.youtube.com/watch?v=dQw4w9WgXcQ") {
            Ok(t) => {
                assert_eq!(t.id, "dQw4w9WgXcQ");
                assert!(t.title.contains("Rick Astley"), "real title, got {:?}", t.title);
                assert!(t.duration_sec > 0, "real duration, got {}", t.duration_sec);
                assert_eq!(t.resolved_by, "yt-dlp");
                assert_ne!(t.duration_sec, 180, "the old fake 180 s must be gone");
            }
            Err(e) => assert!(e.contains("yt-dlp"), "honest error names yt-dlp: {}", e),
        }
    }

    #[test]
    fn bare_youtube_id_resolve_is_real_or_honest_error() {
        match resolve_query("dQw4w9WgXcQ") {
            Ok(t) => {
                assert_eq!(t.id, "dQw4w9WgXcQ");
                assert!(t.duration_sec > 0);
                assert_eq!(t.resolved_by, "yt-dlp");
            }
            Err(e) => assert!(e.contains("yt-dlp"), "honest error names yt-dlp: {}", e),
        }
    }

    /// A Spotify URL has no metadata source in vc-rust: it is an
    /// honest error, not a fake "Spotify track <id>" placeholder.
    #[test]
    fn spotify_url_resolve_is_engine_unavailable() {
        let err = resolve_query("https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC")
            .expect_err("a Spotify URL must not resolve to a fake track");
        assert!(err.contains("Spotify"), "error names Spotify: {}", err);
    }

    /// Without client credentials the Spotify error names exactly
    /// which env vars are missing (env-gated path, Phase 5 scope
    /// update). The vars are restored afterwards; no other test reads
    /// them, so the brief clear window is benign.
    #[test]
    fn spotify_track_missing_creds_names_the_vars() {
        let saved_id = std::env::var("SPOTIFY_CLIENT_ID").ok();
        let saved_secret = std::env::var("SPOTIFY_CLIENT_SECRET").ok();
        unsafe {
            std::env::remove_var("SPOTIFY_CLIENT_ID");
            std::env::remove_var("SPOTIFY_CLIENT_SECRET");
        }
        let err = resolve_query("https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC")
            .expect_err("no creds -> no resolve");
        assert!(err.contains("SPOTIFY_CLIENT_ID"), "error names the id var: {}", err);
        assert!(err.contains("SPOTIFY_CLIENT_SECRET"), "error names the secret var: {}", err);
        unsafe {
            match saved_id {
                Some(v) => std::env::set_var("SPOTIFY_CLIENT_ID", v),
                None => std::env::remove_var("SPOTIFY_CLIENT_ID"),
            }
            match saved_secret {
                Some(v) => std::env::set_var("SPOTIFY_CLIENT_SECRET", v),
                None => std::env::remove_var("SPOTIFY_CLIENT_SECRET"),
            }
        }
    }

    /// The Spotify track mapping is pure: title, comma-joined
    /// artists, duration_ms -> seconds, spotify URL. No network.
    #[test]
    fn spotify_track_json_mapping() {
        let v = parse_json(
            r#"{"name":"Never Gonna Give You Up","artists":[{"name":"Rick Astley"}],"duration_ms":213000,"external_urls":{"spotify":"https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC"}}"#,
        )
        .unwrap();
        let t = spotify_track_from_json(&v, "4uLU6hMCjMI75M1A2tKUQC").expect("maps");
        assert_eq!(t.id, "4uLU6hMCjMI75M1A2tKUQC");
        assert_eq!(t.title, "Never Gonna Give You Up");
        assert_eq!(t.artist, "Rick Astley");
        assert_eq!(t.duration_sec, 213);
        assert_eq!(t.source, "spotify");
        assert_eq!(t.resolved_by, "spotify-api");
        assert_eq!(t.url, "https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC");
        // Multi-artist join + missing duration -> 0 (unknown).
        let v2 = parse_json(
            r#"{"name":"Duet","artists":[{"name":"A"},{"name":"B"}],"external_urls":{}}"#,
        )
        .unwrap();
        let t2 = spotify_track_from_json(&v2, "x").expect("maps");
        assert_eq!(t2.artist, "A, B");
        assert_eq!(t2.duration_sec, 0);
    }

    /// Live Spotify resolve: only runs when the creds are actually
    /// set (user-supplied, D6). Skips otherwise — the missing-creds
    /// path above is the provable part from here, and the sandbox
    /// egress proxy additionally rejects the token endpoint, so the
    /// live path needs the user's host too.
    #[test]
    fn live_spotify_track_resolve() {
        if std::env::var("SPOTIFY_CLIENT_ID").is_err()
            || std::env::var("SPOTIFY_CLIENT_SECRET").is_err()
        {
            eprintln!("skipping live Spotify resolve (set SPOTIFY_CLIENT_ID/SECRET)");
            return;
        }
        match resolve_query("https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC") {
            Ok(t) => {
                assert_eq!(t.id, "4uLU6hMCjMI75M1A2tKUQC");
                assert!(t.title.contains("Rick Astley") || t.title.contains("Never Gonna Give You Up"), "real title, got {:?}", t.title);
                assert!(t.duration_sec > 0, "real duration, got {}", t.duration_sec);
                assert_eq!(t.source, "spotify");
                eprintln!("live Spotify resolve OK: {} — {} ({} s)", t.id, t.title, t.duration_sec);
            }
            Err(e) => {
                // Honest failure (e.g. token endpoint rejected): the
                // error must say what failed, never fake a track.
                assert!(e.contains("Spotify"), "error names Spotify: {}", e);
                eprintln!("live Spotify resolve failed honestly: {}", e);
            }
        }
    }

    /// A free-text query never yields a pattern placeholder: either a
    /// real yt-dlp track (real video id, real duration) or an honest
    /// error. The id assertion is the point: `--dump-single-json` on
    /// a search used to return the playlist wrapper whose `id` was
    /// the query string itself.
    #[test]
    fn free_text_query_never_returns_a_fake_track() {
        match resolve_query("rick astley never gonna give you up") {
            Ok(t) => {
                assert!(!t.id.starts_with("pattern-"), "no pattern id: {}", t.id);
                assert_eq!(t.id, "dQw4w9WgXcQ", "first ytsearch hit, got {}", t.id);
                assert!(t.duration_sec > 0, "real duration, got {}", t.duration_sec);
                assert_eq!(t.resolved_by, "yt-dlp");
            }
            Err(e) => assert!(e.contains("yt-dlp"), "honest error names yt-dlp: {}", e),
        }
    }

    /// The ticker decision: unknown-duration tracks never advance,
    /// however far past their (unknown) end the position is.
    #[test]
    fn due_for_advance_never_fires_for_unknown_duration() {
        let mut st = State::default();
        let chat_id = 42;
        let room = st.room_mut(chat_id);
        let mut t = mk_track("x");
        t.duration_sec = 0; // unknown duration
        t.downloaded = true;
        room.current = Some(t);
        room.position_sec = 9999; // far "past the end"
        assert!(!due_for_advance(&st.rooms[&chat_id]), "duration 0 must never advance");

        // Same room with a known duration: advances at the end.
        st.rooms.get_mut(&chat_id).unwrap().current.as_mut().unwrap().duration_sec = 180;
        st.rooms.get_mut(&chat_id).unwrap().position_sec = 180;
        assert!(due_for_advance(&st.rooms[&chat_id]), "known duration at end must advance");

        // Paused: no advance even at the end.
        st.rooms.get_mut(&chat_id).unwrap().paused = true;
        assert!(!due_for_advance(&st.rooms[&chat_id]), "paused must not advance");
        st.rooms.get_mut(&chat_id).unwrap().paused = false;

        // Still downloading: no advance even at the end.
        let cur = st.rooms.get_mut(&chat_id).unwrap().current.as_mut().unwrap();
        cur.downloaded = false;
        cur.download_failed = false;
        assert!(!due_for_advance(&st.rooms[&chat_id]), "downloading must not advance");
    }

    /// A persisted track without a duration restores as unknown (0),
    /// never the old fake 180.
    #[test]
    fn track_from_json_missing_duration_defaults_zero() {
        let v = parse_json(r#"{"id":"x","title":"T","source":"youtube"}"#).unwrap();
        let t = track_from_json(&v).expect("track parses");
        assert_eq!(t.duration_sec, 0);
    }

    /// An autoplay mix entry without a duration yields a 0-duration
    /// pick (unknown, never auto-advanced) — not the old fake 180.
    #[test]
    fn autoplay_pick_entry_without_duration_is_zero() {
        let entries = vec![Json::Obj(vec![
            ("id".into(), Json::Str("fresh1".into())),
            ("title".into(), Json::Str("Fresh One".into())),
        ])];
        let pick = first_unplayed_entry(&entries, "last-id", &[]).expect("a pick");
        assert_eq!(pick.id, "fresh1");
        assert_eq!(pick.duration_sec, 0);
    }

    /// HTTP surface: /v1/play with an unresolvable query answers 502
    /// engine_unavailable (Spotify URL) and queues nothing.
    #[test]
    fn handle_play_unresolvable_query_is_502_engine_unavailable() {
        let state = Arc::new(Mutex::new(State::default()));
        let req = Request {
            method: "POST".to_string(),
            path: "/v1/play".to_string(),
            query: String::new(),
            headers: vec![],
            body: r#"{"chat_id":4242,"query":"https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC"}"#.to_string(),
        };
        let (status, body) = handle(&state, &req);
        assert_eq!(status, 502, "body: {}", body);
        assert!(body.contains("engine_unavailable"), "body: {}", body);
        assert!(body.contains("Spotify"), "body: {}", body);
        // Nothing was queued: the room has no playback state.
        let st = lock(&state);
        assert!(st.rooms.get(&4242).is_none(), "no room must be created for a failed resolve");
    }
}
