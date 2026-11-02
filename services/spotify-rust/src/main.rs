//! parlay-spotify-rust — Spotify watch + skip-check (Parlay IPC
//! `spotify.*`).
//!
//! Behaviour ported (re-implemented from scratch, not copied) from
//! refs/rustify:
//!   `src/profanity.rs` — profanity check over text (rustify uses the
//!                        rustrict trie; here it is a curated wordlist
//!                        with word-boundary matching, std-only)
//!   `src/lyrics/lrclib.rs` — LRCLib lyrics fetch (`plainLyrics`)
//!   `src/lyrics/{genius,musixmatch}.rs` — credentialed lyrics providers
//!   `src/spotify/*` — Spotify playback monitoring (OAuth + Web API)
//!   `src/queue/track_check.rs` — skip decision: profanity / disliked
//!
//! What is REAL here:
//!   * watch registry — `POST /v1/spotify/watch` start/stop per user,
//!     in-memory + JSON-file persistence (`data/spotify-state.json`)
//!   * dislike store — `POST /v1/spotify/dislike` (add/remove per user);
//!     skip-check returns `skip:true, reason:"disliked"` for them
//!   * profanity filter — curated English wordlist, case-insensitive,
//!     word-boundary match over title + artist (+ LRCLib lyrics when
//!     reachable); reports the matched word
//!   * lyrics check — real LRCLib fetch via a `curl` subprocess with a
//!     5s cap; when LRCLib is unreachable the response says
//!     `lyrics_available:false` and falls back to the title/artist
//!     check — a lyrics verdict is never faked
//!   * Spotify link resolve — `POST /v1/spotify/resolve` parses
//!     `open.spotify.com/<kind>/<id>` and `spotify:<kind>:<id>` URLs
//!     into a track id. No API call.
//!
//! What is NOT done here (needs credentials / a classifier):
//!   * real Spotify playback monitoring — needs SPOTIFY_CLIENT_ID /
//!     SPOTIFY_CLIENT_SECRET plus a per-user OAuth token (rustify
//!     `src/spotify/auth.rs`). The watch registry here records intent
//!     only; nothing polls the Spotify Web API for a user's currently
//!     playing track. What IS real since Phase 4: the app-only
//!     client-credentials flow (`POST /v1/spotify/resolve` enriches a
//!     track link with real title/artist/duration/album, and
//!     `POST /v1/spotify/collection` returns real album/playlist/
//!     artist track lists from the Web API). Without credentials both
//!     say so and never fabricate a track list.
//!   * Genius / Musixmatch lyrics — need GENIUS_ACCESS_TOKEN /
//!     MUSIXMATCH_API_KEY.
//!   * AI-generated-lyrics detection — rustify flags tracks whose
//!     lyrics look AI-written; there is no classifier in this service,
//!     so `reason:"ai-generated"` is never emitted. The field stays in
//!     the contract for a future detector.

mod common;

use common::*;
use std::collections::{BTreeMap, BTreeSet};
use std::process::Command;
use std::sync::{Arc, Mutex};

const SERVICE: &str = "spotify-rust";
const STATE_FILE: &str = "spotify-state.json";
/// LRCLib search cap: one request, fail fast, never block a skip-check.
const LYRICS_TIMEOUT_SECS: &str = "5";

// ---------------------------------------------------------- profanity ---

/// Curated English profanity list (the wordlist concept from rustify
/// `src/profanity.rs`, which uses the rustrict crate's trie —
/// re-implemented here as a plain sorted list, std-only).
///
/// Mild words (hell, damn, goddamn, crap, jesus, queer) are DELIBERATELY
/// excluded: they appear in clean classics ("Imagine" — "no hell below
/// us"; "Highway to Hell") and flagging those is worse than missing a
/// mild word. The list is the clearly-profane tier only.
///
/// Matching is word-boundary on lower-cased tokens, so "class" does not
/// trip on "ass" and "shipment" does not trip on "shit". Deliberately
/// moderate: common mild expletives are in, everyday words that merely
/// contain a bad substring are out. Obfuscated spellings (l33t, spaced
/// letters) are NOT caught — documented in the README.
const PROFANITY: &[&str] = &[
    "anal", "anus", "arse", "arsehole", "ass", "asshole", "bastard", "bitch",
    "bitches", "bitching", "blowjob", "bollocks", "boob", "boobs", "chink", "clitoris",
    "cock", "cocksucker", "cunt", "dick", "dickhead", "dildo", "dyke", "fag",
    "faggot", "fuck", "fucked", "fucker", "fucking", "homo", "jizz", "kike",
    "masturbate", "masturbating", "motherfucker", "nigga", "nigger", "orgasm", "piss", "pissed",
    "porn", "porno", "prick", "pussy", "shit", "shitty", "slut", "spic",
    "tits", "twat", "vagina", "wank", "wanker", "whore",
];

/// Case-insensitive word-boundary profanity check. Returns the first
/// matched word (lower-cased), or None.
fn profanity_check(text: &str) -> Option<String> {
    for token in text.split(|c: char| !c.is_alphanumeric()) {
        if token.is_empty() {
            continue;
        }
        let lower = token.to_lowercase();
        if PROFANITY.binary_search(&lower.as_str()).is_ok() {
            return Some(lower);
        }
    }
    None
}

// -------------------------------------------------------------- state ---

#[derive(Debug, Default)]
struct State {
    /// user_id -> unix seconds when the watch started.
    watches: BTreeMap<i64, i64>,
    /// user_id -> disliked track ids.
    dislikes: BTreeMap<i64, BTreeSet<String>>,
}

impl State {
    fn to_json(&self) -> Json {
        let watches = Json::Arr(
            self.watches
                .iter()
                .map(|(u, started)| {
                    Json::Obj(vec![
                        ("user_id".into(), Json::Num(*u as f64)),
                        ("started_at".into(), Json::Num(*started as f64)),
                    ])
                })
                .collect(),
        );
        let dislikes = Json::Arr(
            self.dislikes
                .iter()
                .filter(|(_, ids)| !ids.is_empty())
                .map(|(u, ids)| {
                    Json::Obj(vec![
                        ("user_id".into(), Json::Num(*u as f64)),
                        (
                            "track_ids".into(),
                            Json::Arr(
                                ids.iter().map(|id| Json::Str(id.clone())).collect(),
                            ),
                        ),
                    ])
                })
                .collect(),
        );
        Json::Obj(vec![("watches".into(), watches), ("dislikes".into(), dislikes)])
    }

    fn from_json(v: &Json) -> State {
        let mut st = State::default();
        if let Some(Json::Arr(items)) = v.get("watches") {
            for it in items {
                if let (Some(u), Some(s)) = (
                    it.get("user_id").and_then(|x| x.as_i64()),
                    it.get("started_at").and_then(|x| x.as_i64()),
                ) {
                    st.watches.insert(u, s);
                }
            }
        }
        if let Some(Json::Arr(items)) = v.get("dislikes") {
            for it in items {
                if let Some(u) = it.get("user_id").and_then(|x| x.as_i64()) {
                    let mut set = BTreeSet::new();
                    if let Some(Json::Arr(ids)) = it.get("track_ids") {
                        for id in ids {
                            if let Some(s) = id.as_str() {
                                set.insert(s.to_string());
                            }
                        }
                    }
                    if !set.is_empty() {
                        st.dislikes.insert(u, set);
                    }
                }
            }
        }
        st
    }

    /// Is this track id disliked by the given user (or by anyone, when
    /// no user is given)? Returns the disliking user id, if any.
    fn disliked_by(&self, track_id: &str, user_id: Option<i64>) -> Option<i64> {
        match user_id {
            Some(u) => self
                .dislikes
                .get(&u)
                .filter(|set| set.contains(track_id))
                .map(|_| u),
            None => self
                .dislikes
                .iter()
                .find(|(_, set)| set.contains(track_id))
                .map(|(u, _)| *u),
        }
    }
}

fn save_state(st: &State) {
    let path = data_file(STATE_FILE);
    if let Err(e) = atomic_write(&path, &st.to_json().render()) {
        eprintln!(
            "[spotify-rust] failed to persist state to {}: {}",
            path.display(),
            e
        );
    }
}

fn load_state() -> State {
    let path = data_file(STATE_FILE);
    match std::fs::read_to_string(&path) {
        Ok(text) => match parse_json(&text) {
            Some(v) => {
                let st = State::from_json(&v);
                eprintln!(
                    "[spotify-rust] restored {} watch(es), {} dislike list(s) from {}",
                    st.watches.len(),
                    st.dislikes.len(),
                    path.display()
                );
                st
            }
            None => {
                eprintln!(
                    "[spotify-rust] WARNING: corrupt state file {}, starting empty",
                    path.display()
                );
                State::default()
            }
        },
        Err(_) => State::default(),
    }
}

// -------------------------------------------------------------- lyrics ---

fn percent_encode(s: &str) -> String {
    let mut out = String::new();
    for b in s.bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                out.push(b as char)
            }
            _ => out.push_str(&format!("%{:02X}", b)),
        }
    }
    out
}

/// Fetch plain lyrics from LRCLib (rustify `src/lyrics/lrclib.rs` does
/// the same search over HTTP). Returns None when the network fails, the
/// request times out, the track is not found, or the result has no
/// plain lyrics. Callers must treat None as "lyrics unavailable", never
/// as a clean verdict.
fn fetch_lrclib_lyrics(title: &str, artist: &str) -> Option<String> {
    let url = format!(
        "https://lrclib.net/api/search?track_name={}&artist_name={}",
        percent_encode(title),
        percent_encode(artist)
    );
    let out = Command::new("curl")
        .arg("-s")
        .arg("--max-time")
        .arg(LYRICS_TIMEOUT_SECS)
        .arg(&url)
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&out.stdout);
    let v = parse_json(text.trim())?;
    let first = match &v {
        Json::Arr(items) => items.first()?,
        _ => return None,
    };
    first
        .get("plainLyrics")
        .and_then(|x| x.as_str())
        .filter(|s| !s.trim().is_empty())
        .map(|s| s.to_string())
}

// ------------------------------------------------- spotify web api ---
//
// Real Spotify Web API client, client-credentials flow
// (rustify uses per-user OAuth: `src/spotify/auth.rs` +
// `src/spotify/mod.rs::Manager`; this service has no user tokens, so it
// uses the app-only flow — enough for track/album/playlist metadata and
// track lists, which is what `spotify.resolve` / `spotify.collection`
// need. No per-user playback state: that still needs a user OAuth token,
// which this service never sees.)
//
// Credentials come from the environment only: SPOTIFY_CLIENT_ID and
// SPOTIFY_CLIENT_SECRET. When they are absent the API functions return
// None and the handlers say so explicitly — a track list is never
// faked; callers fall back to the downloaders service (yt-dlp/oEmbed).

/// Cached app token: (access_token, expiry unix seconds).
type TokenCache = Arc<Mutex<Option<(String, i64)>>>;

fn spotify_creds() -> Option<(String, String)> {
    let id = std::env::var("SPOTIFY_CLIENT_ID").ok()?;
    let secret = std::env::var("SPOTIFY_CLIENT_SECRET").ok()?;
    if !creds_usable(&id, &secret) {
        return None;
    }
    Some((id, secret))
}

/// Pure helper (tested): credentials are usable only when both parts
/// are present and non-empty.
fn creds_usable(id: &str, secret: &str) -> bool {
    !id.is_empty() && !secret.is_empty()
}

fn token_still_valid(cached: &Option<(String, i64)>, now: i64) -> bool {
    match cached {
        Some((_, exp)) => *exp > now + 60,
        None => false,
    }
}

/// Fetch (and cache) a client-credentials access token.
/// `POST https://accounts.spotify.com/api/token` with HTTP Basic auth
/// `client_id:client_secret`, `grant_type=client_credentials`.
/// Returns None when creds are absent or the token endpoint errors.
fn fetch_app_token(cache: &TokenCache) -> Option<String> {
    let now = now_unix();
    {
        let guard = cache.lock().unwrap_or_else(|e| e.into_inner());
        if token_still_valid(&guard, now) {
            return guard.as_ref().map(|(t, _)| t.clone());
        }
    }
    let (id, secret) = spotify_creds()?;
    let out = Command::new("curl")
        .arg("-sS")
        .arg("--max-time")
        .arg("15")
        .arg("-u")
        .arg(format!("{}:{}", id, secret))
        .arg("-d")
        .arg("grant_type=client_credentials")
        .arg("https://accounts.spotify.com/api/token")
        .output()
        .ok()?;
    if !out.status.success() {
        eprintln!(
            "[spotify-rust] token endpoint failed: {}",
            String::from_utf8_lossy(&out.stderr).trim()
        );
        return None;
    }
    let text = String::from_utf8_lossy(&out.stdout);
    let v = parse_json(text.trim())?;
    let token = v.get("access_token")?.as_str()?.to_string();
    let expires_in = v.get("expires_in").and_then(|x| x.as_i64()).unwrap_or(3600);
    {
        let mut guard = cache.lock().unwrap_or_else(|e| e.into_inner());
        *guard = Some((token.clone(), now + expires_in));
    }
    Some(token)
}

/// GET one Spotify Web API resource. Returns None on any failure
/// (no token, curl error, HTTP >= 400 via -f, unparsable JSON).
fn spotify_api_get(cache: &TokenCache, path: &str) -> Option<Json> {
    let token = fetch_app_token(cache)?;
    let url = format!("https://api.spotify.com/v1{}", path);
    let out = Command::new("curl")
        .arg("-sS")
        .arg("-f")
        .arg("--max-time")
        .arg("15")
        .arg("-H")
        .arg(format!("Authorization: Bearer {}", token))
        .arg(&url)
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&out.stdout);
    parse_json(text.trim())
}

/// Map a Spotify track object onto the contract Track shape
/// (+ `album` name and `thumbnail` from the smallest cover image).
fn spotify_track_to_json(v: &Json) -> Option<Json> {
    let id = v.get("id")?.as_str()?.to_string();
    let title = v.get("name").and_then(|x| x.as_str()).unwrap_or("").to_string();
    let artists: Vec<String> = match v.get("artists") {
        Some(Json::Arr(items)) => items
            .iter()
            .filter_map(|a| a.get("name").and_then(|x| x.as_str()).map(|s| s.to_string()))
            .collect(),
        _ => Vec::new(),
    };
    let artist = artists.join(", ");
    let duration_sec = v
        .get("duration_ms")
        .and_then(|x| x.as_i64())
        .map(|ms| ms / 1000)
        .unwrap_or(0);
    let url = v
        .get("external_urls")
        .and_then(|x| x.get("spotify"))
        .and_then(|x| x.as_str())
        .unwrap_or("")
        .to_string();
    let album = v
        .get("album")
        .and_then(|x| x.get("name"))
        .and_then(|x| x.as_str())
        .unwrap_or("")
        .to_string();
    let thumbnail = v
        .get("album")
        .and_then(|x| x.get("images"))
        .and_then(|x| match x {
            Json::Arr(items) => items.last().and_then(|i| i.get("url")).and_then(|u| u.as_str()).map(|s| s.to_string()),
            _ => None,
        });
    let mut fields = vec![
        ("id".to_string(), Json::Str(id)),
        ("title".to_string(), Json::Str(title)),
        ("source".to_string(), Json::Str("spotify".to_string())),
        ("artist".to_string(), Json::Str(artist)),
        ("duration_sec".to_string(), Json::Num(duration_sec as f64)),
        ("url".to_string(), Json::Str(url)),
        ("album".to_string(), Json::Str(album)),
    ];
    if let Some(t) = thumbnail {
        fields.push(("thumbnail".to_string(), Json::Str(t)));
    }
    Some(Json::Obj(fields))
}

/// Fetch one track's real metadata from the Web API.
fn spotify_fetch_track(cache: &TokenCache, id: &str) -> Option<Json> {
    let v = spotify_api_get(cache, &format!("/tracks/{}", id))?;
    spotify_track_to_json(&v)
}

/// Fetch a collection's real track list from the Web API.
/// Returns (display name, tracks, total, truncated). Playlists page
/// through `/playlists/<id>/tracks` (100/page, capped at 100 tracks —
/// the downloaders service caps at 50 per collection anyway);
/// albums come back whole from `/albums/<id>`; artists return their
/// top tracks (`/artists/<id>/top-tracks?market=US`, max 10).
fn spotify_fetch_collection(
    cache: &TokenCache,
    kind: &str,
    id: &str,
) -> Option<(String, Vec<Json>, i64, bool)> {
    match kind {
        "track" => {
            let t = spotify_fetch_track(cache, id)?;
            let name = t
                .get("title")
                .and_then(|x| x.as_str())
                .unwrap_or("")
                .to_string();
            Some((name, vec![t], 1, false))
        }
        "album" => {
            let v = spotify_api_get(cache, &format!("/albums/{}", id))?;
            let name = v
                .get("name")
                .and_then(|x| x.as_str())
                .unwrap_or("")
                .to_string();
            let total = v
                .get("tracks")
                .and_then(|x| x.get("total"))
                .and_then(|x| x.as_i64())
                .unwrap_or(0);
            let mut tracks = Vec::new();
            if let Some(Json::Arr(items)) = v.get("tracks").and_then(|x| x.get("items")) {
                for item in items {
                    if let Some(t) = spotify_track_to_json(item) {
                        tracks.push(t);
                    }
                }
            }
            Some((name, tracks, total, false))
        }
        "playlist" => {
            let v = spotify_api_get(cache, &format!("/playlists/{}", id))?;
            let name = v
                .get("name")
                .and_then(|x| x.as_str())
                .unwrap_or("")
                .to_string();
            let total = v
                .get("tracks")
                .and_then(|x| x.get("total"))
                .and_then(|x| x.as_i64())
                .unwrap_or(0);
            let mut tracks = Vec::new();
            let mut offset = 0;
            let mut truncated = false;
            while (tracks.len() as i64) < total.min(100) {
                let page = spotify_api_get(
                    cache,
                    &format!("/playlists/{}/tracks?limit=100&offset={}", id, offset),
                )?;
                let items = match page.get("items") {
                    Some(Json::Arr(items)) => items.clone(),
                    _ => break,
                };
                if items.is_empty() {
                    break;
                }
                for item in &items {
                    if let Some(track) = item.get("track") {
                        if let Some(t) = spotify_track_to_json(track) {
                            tracks.push(t);
                        }
                    }
                }
                offset += items.len() as i64;
                if (items.len() as i64) < 100 {
                    break;
                }
            }
            if total > 100 {
                truncated = true;
            }
            Some((name, tracks, total, truncated))
        }
        "artist" => {
            let v = spotify_api_get(cache, &format!("/artists/{}", id))?;
            let name = v
                .get("name")
                .and_then(|x| x.as_str())
                .unwrap_or("")
                .to_string();
            let top = spotify_api_get(cache, &format!("/artists/{}/top-tracks?market=US", id))?;
            let mut tracks = Vec::new();
            if let Some(Json::Arr(items)) = top.get("tracks") {
                for item in items {
                    if let Some(t) = spotify_track_to_json(item) {
                        tracks.push(t);
                    }
                }
            }
            let total = tracks.len() as i64;
            Some((name, tracks, total, false))
        }
        _ => None,
    }
}

// ------------------------------------------------------- spotify links ---

/// Parse a Spotify link into (kind, id). Accepts
/// `https://open.spotify.com/<kind>/<id>` (query/fragment tolerated)
/// and `spotify:<kind>:<id>`. No API call — pure parsing.
fn parse_spotify_link(url: &str) -> Option<(String, String)> {
    let lower = url.to_ascii_lowercase();
    for kind in ["track", "album", "playlist", "artist"] {
        let marker = format!("open.spotify.com/{}/", kind);
        if let Some(p) = lower.find(&marker) {
            let rest = &url[p + marker.len()..];
            let id: String = rest
                .chars()
                .take_while(|c| c.is_ascii_alphanumeric())
                .collect();
            if !id.is_empty() {
                return Some((kind.to_string(), id));
            }
        }
        // URI form: slice the ORIGINAL url (not the lower-cased copy —
        // Spotify ids are case-sensitive base62).
        let uri_marker = format!("spotify:{}:", kind);
        if lower.starts_with(&uri_marker) {
            let rest = &url[uri_marker.len()..];
            let id: String = rest
                .chars()
                .take_while(|c| c.is_ascii_alphanumeric())
                .collect();
            if !id.is_empty() {
                return Some((kind.to_string(), id));
            }
        }
    }
    None
}

// ------------------------------------------------------------ helpers ---

fn lock(state: &Arc<Mutex<State>>) -> std::sync::MutexGuard<'_, State> {
    state.lock().unwrap_or_else(|e| e.into_inner())
}

fn now_unix() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

fn need_user(body: &Json) -> Result<i64, (u16, String)> {
    body.get("user_id")
        .and_then(|v| v.as_i64())
        .ok_or((400, err("bad_request", "missing integer field: user_id")))
}

// ------------------------------------------------------------ handlers ---

fn handle_watch(st: &mut State, body: &Json) -> (u16, String) {
    let action = match body.get("action").and_then(|v| v.as_str()) {
        Some(a) => a.to_string(),
        None => return (400, err("bad_request", "missing string field: action")),
    };
    let user_id = match need_user(body) {
        Ok(u) => u,
        Err(e) => return e,
    };
    match action.as_str() {
        "start" => {
            st.watches.insert(user_id, now_unix());
            eprintln!("[spotify-rust] watch started for user {}", user_id);
            (
                200,
                ok(&format!(
                    "{{\"watching\":true,\"user_id\":{}}}",
                    user_id
                )),
            )
        }
        "stop" => {
            let was = st.watches.remove(&user_id).is_some();
            eprintln!(
                "[spotify-rust] watch stopped for user {} (was watching: {})",
                user_id, was
            );
            (
                200,
                ok(&format!(
                    "{{\"watching\":false,\"user_id\":{},\"was_watching\":{}}}",
                    user_id, was
                )),
            )
        }
        _ => (
            400,
            err("bad_request", "unknown action (start|stop)"),
        ),
    }
}

fn handle_dislike(st: &mut State, body: &Json) -> (u16, String) {
    let action = body
        .get("action")
        .and_then(|v| v.as_str())
        .unwrap_or("add")
        .to_string();
    let user_id = match need_user(body) {
        Ok(u) => u,
        Err(e) => return e,
    };
    let track_id = match body.get("track_id").and_then(|v| v.as_str()) {
        Some(t) if !t.is_empty() => t.to_string(),
        _ => return (400, err("bad_request", "missing string field: track_id")),
    };
    match action.as_str() {
        "add" => {
            st.dislikes
                .entry(user_id)
                .or_default()
                .insert(track_id.clone());
            (
                200,
                ok(&format!(
                    "{{\"disliked\":true,\"user_id\":{},\"track_id\":{}}}",
                    user_id,
                    quote(&track_id)
                )),
            )
        }
        "remove" => {
            let removed = st
                .dislikes
                .get_mut(&user_id)
                .map(|set| set.remove(&track_id))
                .unwrap_or(false);
            (
                200,
                ok(&format!(
                    "{{\"disliked\":false,\"removed\":{},\"user_id\":{},\"track_id\":{}}}",
                    removed,
                    user_id,
                    quote(&track_id)
                )),
            )
        }
        _ => (
            400,
            err("bad_request", "unknown action (add|remove)"),
        ),
    }
}

/// skip-check decision order (rustify `src/queue/track_check.rs`):
/// disliked -> profanity in title/artist -> profanity in LRCLib lyrics.
/// `reason` is one of profanity | disliked | null. `ai-generated` is
/// never emitted: no classifier exists in this service (see README).
fn handle_skip_check(st: &State, body: &Json) -> (u16, String) {
    let track = match body.get("track") {
        Some(t @ Json::Obj(_)) => t,
        _ => {
            return (
                400,
                err("bad_request", "missing object field: track (needs id, title)"),
            )
        }
    };
    let track_id = match track.get("id").and_then(|v| v.as_str()) {
        Some(id) => id.to_string(),
        None => return (400, err("bad_request", "track.id is required")),
    };
    let title = track
        .get("title")
        .and_then(|v| v.as_str())
        .unwrap_or("")
        .to_string();
    let artist = track
        .get("artist")
        .and_then(|v| v.as_str())
        .unwrap_or("")
        .to_string();
    let user_id = body.get("user_id").and_then(|v| v.as_i64());

    // 1. Disliked by this user (or by anyone, when no user is given).
    if let Some(by) = st.disliked_by(&track_id, user_id) {
        return (
            200,
            ok(&format!(
                "{{\"skip\":true,\"reason\":\"disliked\",\"matched_word\":null,\"disliked_by\":{},\"lyrics_checked\":false,\"lyrics_available\":false,\"lyrics_source\":null}}",
                by
            )),
        );
    }

    // 2. Profanity in title / artist.
    let text = format!("{} {}", title, artist);
    if let Some(word) = profanity_check(&text) {
        return (
            200,
            ok(&format!(
                "{{\"skip\":true,\"reason\":\"profanity\",\"matched_word\":{},\"lyrics_checked\":false,\"lyrics_available\":false,\"lyrics_source\":null}}",
                quote(&word)
            )),
        );
    }

    // 3. Profanity in lyrics (LRCLib, real fetch). Unreachable or no
    //    lyrics => lyrics_available:false, no lyrics verdict at all.
    let lyrics = if title.is_empty() {
        None
    } else {
        fetch_lrclib_lyrics(&title, &artist)
    };
    match lyrics {
        Some(lyr) => {
            if let Some(word) = profanity_check(&lyr) {
                return (
                    200,
                    ok(&format!(
                        "{{\"skip\":true,\"reason\":\"profanity\",\"matched_word\":{},\"lyrics_checked\":true,\"lyrics_available\":true,\"lyrics_source\":\"lrclib\"}}",
                        quote(&word)
                    )),
                );
            }
            (
                200,
                ok("{\"skip\":false,\"reason\":null,\"matched_word\":null,\"lyrics_checked\":true,\"lyrics_available\":true,\"lyrics_source\":\"lrclib\"}"),
            )
        }
        None => (
            200,
            ok("{\"skip\":false,\"reason\":null,\"matched_word\":null,\"lyrics_checked\":false,\"lyrics_available\":false,\"lyrics_source\":null}"),
        ),
    }
}

/// `POST /v1/spotify/resolve` — parses the link, then, when
/// SPOTIFY_CLIENT_ID/SECRET are configured, enriches it with real
/// Web API metadata (track title/artist/duration, or the collection
/// name + total). Without credentials it stays parse-only and says so
/// in `note` — no metadata is invented.
fn handle_resolve(_st: &State, tokens: &TokenCache, body: &Json) -> (u16, String) {
    let url = match body.get("url").and_then(|v| v.as_str()) {
        Some(u) if !u.is_empty() => u.to_string(),
        _ => return (400, err("bad_request", "missing string field: url")),
    };
    let (kind, id) = match parse_spotify_link(&url) {
        Some((k, i)) => (k, i),
        None => {
            return (
                400,
                err(
                    "bad_request",
                    "not a Spotify track/album/playlist/artist link (open.spotify.com/... or spotify:<kind>:<id>)",
                ),
            )
        }
    };
    let extra: String = if spotify_creds().is_some() {
        if kind == "track" {
            match spotify_fetch_track(tokens, &id) {
                Some(t) => {
                    format!(",\"track\":{},\"api\":\"spotify-web-api\"", t.render())
                }
                None => {
                    ",\"api\":\"spotify-web-api\",\"api_error\":\"track fetch failed\"".to_string()
                }
            }
        } else {
            match spotify_fetch_collection(tokens, &kind, &id) {
                Some((name, _, total, _)) => {
                    format!(
                        ",\"name\":{},\"total\":{},\"api\":\"spotify-web-api\"",
                        quote(&name),
                        total
                    )
                }
                None => {
                    ",\"api\":\"spotify-web-api\",\"api_error\":\"collection fetch failed\"".to_string()
                }
            }
        }
    } else {
        ",\"api\":\"none\",\"note\":\"no SPOTIFY_CLIENT_ID/SPOTIFY_CLIENT_SECRET configured; metadata not fetched\"".to_string()
    };
    (
        200,
        ok(&format!(
            "{{\"kind\":{},\"track_id\":{},\"url\":{},\"source\":\"spotify\"{}}}",
            quote(&kind),
            quote(&id),
            quote(&url),
            extra
        )),
    )
}

/// `POST /v1/spotify/collection` — real track list for an album,
/// playlist or artist URL, from the Web API. Requires credentials:
/// without them the honest answer is 503 `engine_unavailable`
/// pointing at the downloaders service fallback — a track list is
/// never fabricated.
fn handle_collection(tokens: &TokenCache, body: &Json) -> (u16, String) {
    let url = match body.get("url").and_then(|v| v.as_str()) {
        Some(u) if !u.is_empty() => u.to_string(),
        _ => return (400, err("bad_request", "missing string field: url")),
    };
    let (kind, id) = match parse_spotify_link(&url) {
        Some((k, i)) => (k, i),
        None => {
            return (
                400,
                err(
                    "bad_request",
                    "not a Spotify album/playlist/artist link (open.spotify.com/... or spotify:<kind>:<id>)",
                ),
            )
        }
    };
    if kind == "track" {
        return (
            400,
            err(
                "bad_request",
                "use /v1/spotify/resolve for single track URLs",
            ),
        );
    }
    if spotify_creds().is_none() {
        return (
            503,
            err(
                "engine_unavailable",
                "no SPOTIFY_CLIENT_ID/SPOTIFY_CLIENT_SECRET configured; the downloaders service resolves Spotify collections via yt-dlp/oEmbed instead",
            ),
        );
    }
    match spotify_fetch_collection(tokens, &kind, &id) {
        Some((name, tracks, total, truncated)) => {
            let tracks_json = Json::Arr(tracks).render();
            (
                200,
                ok(&format!(
                    "{{\"kind\":{},\"id\":{},\"name\":{},\"total\":{},\"truncated\":{},\"tracks\":{},\"api\":\"spotify-web-api\"}}",
                    quote(&kind),
                    quote(&id),
                    quote(&name),
                    total,
                    truncated,
                    tracks_json
                )),
            )
        }
        None => (
            502,
            err(
                "download_failed",
                &format!("Spotify API could not resolve {} {}", kind, id),
            ),
        ),
    }
}

/// `GET /v1/spotify/status` — credential/token state (no secrets).
fn handle_status(tokens: &TokenCache) -> (u16, String) {
    let configured = spotify_creds().is_some();
    let cached = {
        let guard = tokens.lock().unwrap_or_else(|e| e.into_inner());
        token_still_valid(&guard, now_unix())
    };
    (
        200,
        ok(&format!(
            "{{\"credentials_configured\":{},\"token_cached\":{},\"api\":\"spotify-web-api\"}}",
            configured, cached
        )),
    )
}

fn handle(state: &Arc<Mutex<State>>, tokens: &TokenCache, req: &Request) -> (u16, String) {
    if req.method == "GET" && req.path == "/v1/spotify/watches" {
        let st = lock(state);
        let ids: Vec<String> = st.watches.keys().map(|u| u.to_string()).collect();
        return (
            200,
            ok(&format!("{{\"watches\":[{}]}}", ids.join(","))),
        );
    }
    if req.method == "GET" && req.path == "/v1/spotify/status" {
        return handle_status(tokens);
    }
    if req.method != "POST" {
        return (404, err("not_found", "route not found"));
    }
    let body = match parse_json(&req.body) {
        Some(b @ Json::Obj(_)) => b,
        _ => return (400, err("bad_request", "invalid JSON body")),
    };
    let mut st = lock(state);
    let res = match req.path.as_str() {
        "/v1/spotify/watch" => handle_watch(&mut st, &body),
        "/v1/spotify/dislike" => handle_dislike(&mut st, &body),
        "/v1/spotify/resolve" => handle_resolve(&st, tokens, &body),
        "/v1/spotify/collection" => handle_collection(tokens, &body),
        "/v1/spotify/skip-check" => handle_skip_check(&st, &body),
        _ => return (404, err("not_found", "route not found")),
    };
    // skip-check and resolve are read-only; saving anyway is harmless
    // and keeps watch/dislike mutations durable on every write path.
    save_state(&st);
    res
}

fn main() {
    let port: u16 = std::env::var("PORT")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(8007);
    let state = Arc::new(Mutex::new(load_state()));
    // App-token cache lives outside the persisted State: the access
    // token is a secret and is never written to disk.
    let tokens: TokenCache = Arc::new(Mutex::new(None));
    let handler_state = Arc::clone(&state);
    let handler_tokens = Arc::clone(&tokens);
    let handler = move |req: &Request| handle(&handler_state, &handler_tokens, req);
    if let Err(e) = run_server(port, SERVICE, handler) {
        eprintln!("parlay {} failed to listen on {}: {}", SERVICE, port, e);
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A real Spotify Web API track object (shape as returned by
    /// GET /v1/tracks/<id>), trimmed to the fields we map.
    const SAMPLE_TRACK_JSON: &str = r#"{
        "id": "4uLU6hMCjMI75M1A2tKUQC",
        "name": "Never Gonna Give You Up",
        "duration_ms": 213573,
        "artists": [{"name": "Rick Astley"}],
        "album": {"name": "Whenever You Need Somebody",
                  "images": [{"url": "https://i.scdn.co/image/big"},
                             {"url": "https://i.scdn.co/image/small"}]},
        "external_urls": {"spotify": "https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC"}
    }"#;

    #[test]
    fn track_mapping_from_api_shape() {
        let v = parse_json(SAMPLE_TRACK_JSON).expect("sample parses");
        let t = spotify_track_to_json(&v).expect("maps");
        assert_eq!(t.get("id").and_then(|x| x.as_str()), Some("4uLU6hMCjMI75M1A2tKUQC"));
        assert_eq!(t.get("title").and_then(|x| x.as_str()), Some("Never Gonna Give You Up"));
        assert_eq!(t.get("artist").and_then(|x| x.as_str()), Some("Rick Astley"));
        assert_eq!(t.get("duration_sec").and_then(|x| x.as_i64()), Some(213));
        assert_eq!(t.get("source").and_then(|x| x.as_str()), Some("spotify"));
        assert_eq!(t.get("album").and_then(|x| x.as_str()), Some("Whenever You Need Somebody"));
        // Thumbnail is the smallest (last) cover image.
        assert_eq!(
            t.get("thumbnail").and_then(|x| x.as_str()),
            Some("https://i.scdn.co/image/small")
        );
    }

    #[test]
    fn track_mapping_multi_artist_and_missing_fields() {
        let v = parse_json(r#"{"id": "abc", "name": "Duet", "duration_ms": 90000,
            "artists": [{"name": "One"}, {"name": "Two"}]}"#).unwrap();
        let t = spotify_track_to_json(&v).unwrap();
        assert_eq!(t.get("artist").and_then(|x| x.as_str()), Some("One, Two"));
        assert_eq!(t.get("duration_sec").and_then(|x| x.as_i64()), Some(90));
        assert_eq!(t.get("album").and_then(|x| x.as_str()), Some(""));
        assert!(t.get("thumbnail").is_none());
    }

    #[test]
    fn track_mapping_requires_id() {
        let v = parse_json(r#"{"name": "No Id"}"#).unwrap();
        assert!(spotify_track_to_json(&v).is_none());
    }

    #[test]
    fn creds_usable_needs_both_parts() {
        assert!(creds_usable("id", "secret"));
        assert!(!creds_usable("", "secret"));
        assert!(!creds_usable("id", ""));
        assert!(!creds_usable("", ""));
    }

    #[test]
    fn token_validity_window() {
        let now = 1_000_000;
        assert!(token_still_valid(&Some(("tok".to_string(), now + 3600)), now));
        assert!(!token_still_valid(&Some(("tok".to_string(), now + 30)), now));
        assert!(!token_still_valid(&Some(("tok".to_string(), now - 10)), now));
        assert!(!token_still_valid(&None, now));
    }

    #[test]
    fn link_parse_still_works() {
        assert_eq!(
            parse_spotify_link("https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC?si=x"),
            Some(("track".to_string(), "4uLU6hMCjMI75M1A2tKUQC".to_string()))
        );
        assert_eq!(
            parse_spotify_link("spotify:playlist:37i9dQZF1DXcBWIGoYBM5M"),
            Some(("playlist".to_string(), "37i9dQZF1DXcBWIGoYBM5M".to_string()))
        );
        assert_eq!(parse_spotify_link("https://example.com/nope"), None);
    }

    /// Live API test: real client-credentials token + real track fetch.
    /// Runs only when SPOTIFY_CLIENT_ID/SECRET are set in the test
    /// environment; otherwise it reports the skip and passes (no
    /// credentials must ever be fabricated or read from disk).
    #[test]
    fn live_spotify_api_track() {
        if spotify_creds().is_none() {
            eprintln!("skipping live Spotify API test (no SPOTIFY_CLIENT_ID/SECRET in env)");
            return;
        }
        let tokens: TokenCache = Arc::new(Mutex::new(None));
        let token = fetch_app_token(&tokens).expect("token endpoint returns a token");
        assert!(!token.is_empty());
        let track = spotify_fetch_track(&tokens, "4uLU6hMCjMI75M1A2tKUQC")
            .expect("real track fetch succeeds");
        assert_eq!(
            track.get("title").and_then(|x| x.as_str()),
            Some("Never Gonna Give You Up")
        );
        eprintln!(
            "live Spotify API OK: {} — {} ({}s)",
            track.get("id").and_then(|x| x.as_str()).unwrap_or("?"),
            track.get("title").and_then(|x| x.as_str()).unwrap_or("?"),
            track.get("duration_sec").and_then(|x| x.as_i64()).unwrap_or(-1)
        );
    }

    /// Live API test: a real public playlist resolves to a real,
    /// non-empty track list. Same credential gate as above.
    #[test]
    fn live_spotify_api_playlist() {
        if spotify_creds().is_none() {
            eprintln!("skipping live Spotify playlist test (no credentials in env)");
            return;
        }
        let tokens: TokenCache = Arc::new(Mutex::new(None));
        let (name, tracks, total, truncated) =
            spotify_fetch_collection(&tokens, "playlist", "37i9dQZF1DXcBWIGoYBM5M")
                .expect("real playlist fetch succeeds");
        assert!(!tracks.is_empty(), "playlist returned zero tracks");
        assert!(total >= tracks.len() as i64);
        eprintln!(
            "live playlist OK: {:?} — {} tracks fetched of {} total (truncated={})",
            name,
            tracks.len(),
            total,
            truncated
        );
    }
}
