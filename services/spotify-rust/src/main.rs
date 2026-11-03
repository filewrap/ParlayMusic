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
//!     (ported against the public Genius / Musixmatch APIs, env-gated:
//!     GENIUS_ACCESS_TOKEN / MUSIXMATCH_API_KEY; Genius → Musixmatch →
//!     LRCLib fallback in `fetch_lyrics`)
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
//!   * lyrics check — real fetch via a `curl` subprocess with a
//!     5s cap, provider chain Genius → Musixmatch → LRCLib (the
//!     first two env-gated by their keys); when no provider is
//!     reachable the response says `lyrics_available:false` and
//!     falls back to the title/artist check — a lyrics verdict is
//!     never faked
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
//!   * Genius / Musixmatch lyrics — wired (Phase 5 scope update):
//!     Genius public search API + song-page scrape, Musixmatch
//!     `matcher.lyrics.get`; both env-gated (GENIUS_ACCESS_TOKEN /
//!     MUSIXMATCH_API_KEY) and falling back to LRCLib; without keys
//!     the chain is LRCLib only.
//!   * AI-generated-lyrics detection — rustify flags tracks whose
//!     lyrics look AI-written; there is no classifier in this service,
//!     so `reason:"ai-generated"` is never emitted. The field stays in
//!     the contract for a future detector.
//!
//! Phase 5 (2026-11-03) closed the three gaps above — see README.md:
//! Gemini classifier (`src/classifier.rs`), per-user OAuth tokens
//! encrypted at rest + currently-playing polling (`src/auth.rs`,
//! `src/crypto.rs`), and the poller thread in `main()`.

mod auth;
mod classifier;
mod common;
mod crypto;
mod store;

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

/// Case-insensitive word-boundary profanity scan. Returns every
/// matched word (lower-cased, deduped, in first-appearance order) —
/// the analyze endpoint reports the full list, while `profanity_check`
/// (below) keeps returning only the first match for the skip path.
fn profanity_words(text: &str) -> Vec<String> {
    let mut found: Vec<String> = Vec::new();
    for token in text.split(|c: char| !c.is_alphanumeric()) {
        if token.is_empty() {
            continue;
        }
        let lower = token.to_lowercase();
        if PROFANITY.binary_search(&lower.as_str()).is_ok()
            && !found.contains(&lower)
        {
            found.push(lower);
        }
    }
    found
}

/// Case-insensitive word-boundary profanity check. Returns the first
/// matched word (lower-cased), or None.
fn profanity_check(text: &str) -> Option<String> {
    profanity_words(text).into_iter().next()
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
    // Shared store (A11): the whole state is one value under key
    // `spotify:state`. The JSON-file path below is unchanged.
    if store::backend() != "file" {
        if let Err(e) = store::set("spotify:state", &st.to_json().render()) {
            eprintln!("[spotify-rust] shared store set failed: {}", e);
        }
        return;
    }
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
    // Shared store (A11): same JSON shape as spotify-state.json.
    if store::backend() != "file" {
        return match store::get("spotify:state") {
            Ok(Some(text)) => match parse_json(&text) {
                Some(v) => {
                    let st = State::from_json(&v);
                    eprintln!(
                        "[spotify-rust] restored {} watch(es), {} dislike list(s) from shared store ({})",
                        st.watches.len(),
                        st.dislikes.len(),
                        store::describe()
                    );
                    st
                }
                None => {
                    eprintln!("[spotify-rust] WARNING: corrupt shared state, starting empty");
                    State::default()
                }
            },
            Ok(None) => State::default(),
            Err(e) => {
                eprintln!("[spotify-rust] shared store load failed: {}", e);
                State::default()
            }
        };
    }
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

/// Genius lyrics (rustify `src/lyrics/genius.rs`, re-implemented against
/// the PUBLIC Genius API — the ref talks to a self-hosted Genius proxy
/// service that does not exist in Parlay, so this port uses
/// api.genius.com directly; the only missing piece is the key).
///
/// Flow: `GET https://api.genius.com/search?q=<title artist>` with
/// `Authorization: Bearer $GENIUS_ACCESS_TOKEN`, pick the best hit, then
/// fetch the song page and scrape the `data-lyrics-container="true"`
/// divs (the standard Genius page shape). Returns None when the key is
/// unset, the network fails, no hit matches, or the page has no lyrics
/// container — never faked. The token is passed to curl as an argv
/// `-H` value only, never logged and never stored.
fn genius_configured() -> bool {
    std::env::var("GENIUS_ACCESS_TOKEN")
        .map(|v| !v.trim().is_empty())
        .unwrap_or(false)
}

fn fetch_genius_lyrics(title: &str, artist: &str) -> Option<String> {
    let token = std::env::var("GENIUS_ACCESS_TOKEN").ok()?;
    if token.trim().is_empty() {
        return None;
    }
    fetch_genius_lyrics_with(title, artist, token.trim())
}

/// Genius lyrics + the song-page URL they came from. The URL is the
/// details `lyrics_link` (the core renders it as the "Lyrics" button);
/// the plain `fetch_genius_lyrics_with` below keeps the old
/// lyrics-only shape for the skip path.
fn fetch_genius_lyrics_with_url(title: &str, artist: &str, token: &str) -> Option<(String, String)> {
    let q = format!("{} {}", title, artist);
    let url = format!(
        "https://api.genius.com/search?q={}",
        percent_encode(q.trim())
    );
    let out = Command::new("curl")
        .arg("-s")
        .arg("--max-time")
        .arg(LYRICS_TIMEOUT_SECS)
        .arg("-H")
        .arg(format!("Authorization: Bearer {}", token))
        .arg(&url)
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&out.stdout);
    let v = parse_json(text.trim())?;
    let page_url = pick_genius_hit(&v, artist)?;
    let page = Command::new("curl")
        .arg("-s")
        .arg("--max-time")
        .arg(LYRICS_TIMEOUT_SECS)
        .arg("-A")
        .arg("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36")
        .arg(&page_url)
        .output()
        .ok()?;
    if !page.status.success() {
        return None;
    }
    let lyrics = extract_genius_lyrics_html(&String::from_utf8_lossy(&page.stdout))?;
    Some((lyrics, page_url))
}

fn fetch_genius_lyrics_with(title: &str, artist: &str, token: &str) -> Option<String> {
    fetch_genius_lyrics_with_url(title, artist, token).map(|(lyrics, _)| lyrics)
}

/// Pick the Genius song-page URL from a public-API search response
/// (`response.hits[].result`): the first hit whose primary artist
/// matches (case-insensitive substring either way), else the first hit
/// with a URL, else None.
fn pick_genius_hit(v: &Json, artist: &str) -> Option<String> {
    let hits = v.get("response")?.get("hits")?;
    let items = match hits {
        Json::Arr(items) => items,
        _ => return None,
    };
    let want = artist.to_lowercase();
    let mut first: Option<String> = None;
    for hit in items {
        let result = hit.get("result")?;
        let url = result.get("url")?.as_str()?.to_string();
        if first.is_none() {
            first = Some(url.clone());
        }
        let hit_artist = result
            .get("primary_artist")
            .and_then(|a| a.get("name"))
            .and_then(|n| n.as_str())
            .unwrap_or("")
            .to_lowercase();
        if !want.is_empty()
            && !hit_artist.is_empty()
            && (hit_artist.contains(&want) || want.contains(&hit_artist))
        {
            return Some(url);
        }
    }
    first
}

/// Extract lyrics text from a Genius song page: every
/// `<div data-lyrics-container="true">…</div>` block, tags stripped,
/// `<br>` → newline, entities unescaped. Returns None when there is no
/// lyrics container or nothing survives stripping (page shape changed —
/// lyrics unavailable, never faked).
fn extract_genius_lyrics_html(html: &str) -> Option<String> {
    const MARKER: &str = "data-lyrics-container=\"true\"";
    let mut lines: Vec<String> = Vec::new();
    let mut rest = html;
    loop {
        let at = match rest.find(MARKER) {
            Some(i) => i,
            None => break,
        };
        let after = &rest[at..];
        // End of the opening <div ...> tag; content starts after it.
        let tag_end = after.find('>')? + 1;
        let inner = &after[tag_end..];
        // Find the matching </div> with a depth counter (containers nest divs).
        let mut depth: i32 = 1;
        let mut pos = 0usize;
        let mut end: Option<usize> = None;
        while pos < inner.len() {
            let slice = &inner[pos..];
            let open = slice.find("<div");
            let close = slice.find("</div>");
            match (open, close) {
                (Some(o), Some(c)) if o < c => {
                    depth += 1;
                    pos += o + 4;
                }
                (_, Some(c)) => {
                    depth -= 1;
                    if depth == 0 {
                        end = Some(pos + c);
                        break;
                    }
                    pos += c + 6;
                }
                _ => break,
            }
        }
        let block = match end {
            Some(e) => &inner[..e],
            None => break,
        };
        // <br> variants become newlines before the remaining tags go.
        let with_breaks = block
            .replace("<br>", "\n")
            .replace("<br/>", "\n")
            .replace("<br />", "\n");
        let text = unescape_html_entities(&strip_html_tags(&with_breaks));
        for line in text.lines() {
            let line = line.trim();
            if !line.is_empty() {
                lines.push(line.to_string());
            }
        }
        rest = &inner[end.unwrap_or(inner.len())..];
    }
    if lines.is_empty() {
        None
    } else {
        Some(lines.join("\n"))
    }
}

/// Strip `<...>` tags (comments included) from a snippet.
fn strip_html_tags(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut in_tag = false;
    for ch in s.chars() {
        match ch {
            '<' => in_tag = true,
            '>' => in_tag = false,
            _ if !in_tag => out.push(ch),
            _ => {}
        }
    }
    out
}

/// Unescape the HTML entities Genius pages use in lyrics. `&amp;` is
/// replaced last so `&amp;lt;` becomes `&lt;`, not `<`.
fn unescape_html_entities(s: &str) -> String {
    s.replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&quot;", "\"")
        .replace("&#39;", "'")
        .replace("&apos;", "'")
        .replace("&nbsp;", " ")
        .replace("&amp;", "&")
}

/// Musixmatch lyrics (rustify `src/lyrics/musixmatch.rs`, re-implemented
/// against the PUBLIC Musixmatch API — the ref uses the desktop endpoint
/// with rotating usertokens; this port uses `matcher.lyrics.get` with
/// `$MUSIXMATCH_API_KEY`, the credential the env var names).
///
/// Returns None when the key is unset, the network fails, the API
/// status is not 200, the track is restricted (free tier returns partial
/// lyrics — reported as unavailable rather than classified on a
/// fragment) or instrumental, or the body is empty. Never faked.
fn musixmatch_configured() -> bool {
    std::env::var("MUSIXMATCH_API_KEY")
        .map(|v| !v.trim().is_empty())
        .unwrap_or(false)
}

fn fetch_musixmatch_lyrics(title: &str, artist: &str) -> Option<String> {
    let key = std::env::var("MUSIXMATCH_API_KEY").ok()?;
    if key.trim().is_empty() {
        return None;
    }
    fetch_musixmatch_lyrics_with(title, artist, key.trim())
}

fn fetch_musixmatch_lyrics_with(title: &str, artist: &str, key: &str) -> Option<String> {
    let url = format!(
        "https://api.musixmatch.com/ws/1.1/matcher.lyrics.get?format=json&q_track={}&q_artist={}&apikey={}",
        percent_encode(title),
        percent_encode(artist),
        percent_encode(key)
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
    parse_musixmatch_lyrics(&v)
}

/// Parse a `matcher.lyrics.get` response: status 200 + a non-empty,
/// unrestricted, non-instrumental `lyrics_body`.
fn parse_musixmatch_lyrics(v: &Json) -> Option<String> {
    let message = v.get("message")?;
    let status = message
        .get("header")
        .and_then(|h| h.get("status_code"))
        .and_then(|s| s.as_i64())?;
    if status != 200 {
        return None;
    }
    let lyrics = message.get("body")?.get("lyrics")?;
    let is_set = |key: &str| -> bool {
        match lyrics.get(key) {
            Some(Json::Num(n)) => *n != 0.0,
            Some(Json::Bool(b)) => *b,
            _ => false,
        }
    };
    if is_set("restricted") || is_set("instrumental") {
        return None;
    }
    lyrics
        .get("lyrics_body")
        .and_then(|x| x.as_str())
        .filter(|s| !s.trim().is_empty())
        .map(|s| s.to_string())
}

/// Lyrics fetch with provider fallback, in order: Genius (keyed),
/// Musixmatch (keyed), LRCLib (keyless). Returns the lyrics and which
/// provider supplied them (`lyrics_source` in the skip-check response).
/// No key anywhere → LRCLib only; nothing reachable → (None, None).
fn fetch_lyrics(title: &str, artist: &str) -> (Option<String>, Option<&'static str>) {
    let (lyrics, source, _) = fetch_lyrics_with_url(title, artist);
    (lyrics, source)
}

/// Lyrics + source + the Genius song-page URL when Genius supplied
/// them (the details `lyrics_link`). Musixmatch and LRCLib return no
/// page URL, so the link stays None for those sources.
fn fetch_lyrics_with_url(title: &str, artist: &str) -> (Option<String>, Option<&'static str>, Option<String>) {
    if genius_configured() {
        let token = std::env::var("GENIUS_ACCESS_TOKEN").ok().unwrap_or_default();
        if let Some((lyrics, page_url)) = fetch_genius_lyrics_with_url(title, artist, token.trim()) {
            return (Some(lyrics), Some("genius"), Some(page_url));
        }
    }
    if musixmatch_configured() {
        if let Some(lyrics) = fetch_musixmatch_lyrics(title, artist) {
            return (Some(lyrics), Some("musixmatch"), None);
        }
    }
    match fetch_lrclib_lyrics(title, artist) {
        Some(lyrics) => (Some(lyrics), Some("lrclib"), None),
        None => (None, None, None),
    }
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

/// IPC-3 (§29) human duration — same rendering as vc-go / vc-ts /
/// vc-rust / downloaders / netease-go (asserted identically in each
/// service's tests). A Spotify track is never live.
fn format_duration_str(sec: i64) -> String {
    if sec <= 0 {
        return "∞".to_string();
    }
    if sec < 3600 {
        return format!("{}:{:02}", sec / 60, sec % 60);
    }
    let mut parts = vec![format!("{}h", sec / 3600)];
    if (sec % 3600) / 60 > 0 {
        parts.push(format!("{}m", (sec % 3600) / 60));
    }
    if sec % 60 > 0 {
        parts.push(format!("{}s", sec % 60));
    }
    parts.join(" ")
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
        // IPC-3 (§29): derived human duration, always present.
        ("duration_str".to_string(), Json::Str(format_duration_str(duration_sec))),
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

/// IPC-7 (§29): map a `/audio-features/{id}` response onto the
/// details `features` object. `key` (0-11) becomes the note name,
/// `mode` becomes the modality string. Returns None when the input
/// is not an object — the endpoint is deprecated and answers 403 for
/// most app tokens, so an unavailable features object is `null` in
/// the details response, never faked. A key of -1 (Spotify: "no key
/// detected") renders as "Unknown", the API's own unknown state.
fn audio_features_to_json(v: &Json) -> Option<Json> {
    if !matches!(v, Json::Obj(_)) {
        return None;
    }
    const KEY_NAMES: [&str; 12] = [
        "C", "C#/Db", "D", "D#/Eb", "E", "F", "F#/Gb", "G", "G#/Ab", "A",
        "A#/Bb", "B",
    ];
    let num = |key: &str| -> f64 {
        v.get(key).and_then(|x| x.as_f64()).unwrap_or(0.0)
    };
    let key = match v.get("key").and_then(|x| x.as_i64()) {
        Some(k) if (0..12).contains(&k) => KEY_NAMES[k as usize].to_string(),
        _ => "Unknown".to_string(),
    };
    let modality = match v.get("mode").and_then(|x| x.as_i64()) {
        Some(1) => "Major",
        Some(0) => "Minor",
        _ => "Unknown",
    };
    Some(Json::Obj(vec![
        ("key".into(), Json::Str(key)),
        ("modality".into(), Json::Str(modality.to_string())),
        ("tempo_bpm".into(), Json::Num(num("tempo"))),
        ("acousticness".into(), Json::Num(num("acousticness"))),
        ("danceability".into(), Json::Num(num("danceability"))),
        ("energy".into(), Json::Num(num("energy"))),
        ("instrumentalness".into(), Json::Num(num("instrumentalness"))),
        ("liveness".into(), Json::Num(num("liveness"))),
        ("speechiness".into(), Json::Num(num("speechiness"))),
        ("valence".into(), Json::Num(num("valence"))),
    ]))
}

/// IPC-7: collect genres from an `/artists?ids=` response
/// ({"artists": [{"genres": [...]}]}), deduped, first-seen order.
fn artist_genres(v: &Json) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    if let Some(Json::Arr(artists)) = v.get("artists") {
        for a in artists {
            if let Some(Json::Arr(genres)) = a.get("genres") {
                for g in genres {
                    if let Some(s) = g.as_str() {
                        let s = s.to_string();
                        if !s.is_empty() && !out.contains(&s) {
                            out.push(s);
                        }
                    }
                }
            }
        }
    }
    out
}

/// One-sentence analysis text from a classifier result, for the
/// analyze endpoint: verdict + provider + the classifier's own
/// reason. Pure — the wordlist path is deterministic without keys.
fn analysis_text(result: &classifier::ClassifierResult) -> String {
    format!(
        "Verdict: {} ({}) — {}",
        classifier::verdict_name(&result.verdict),
        result.provider,
        result.detail
    )
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

/// The skip-check decision, shared by the HTTP handler and the
/// currently-playing poller. `disliked_by` is precomputed by the
/// caller (it needs the State lock); everything else here is network
/// I/O (LRCLib + optionally Gemini) and must NOT run under the lock.
struct SkipDecision {
    skip: bool,
    reason: Option<String>,
    matched_word: Option<String>,
    disliked_by: Option<i64>,
    lyrics_checked: bool,
    lyrics_available: bool,
    lyrics_source: Option<String>,
    classifier_provider: String,
    classifier_verdict: String,
    classifier_confidence: Option<f64>,
    classifier_detail: String,
}

impl SkipDecision {
    fn clean(classifier_provider: &str, classifier_verdict: &str, detail: &str) -> SkipDecision {
        SkipDecision {
            skip: false,
            reason: None,
            matched_word: None,
            disliked_by: None,
            lyrics_checked: false,
            lyrics_available: false,
            lyrics_source: None,
            classifier_provider: classifier_provider.to_string(),
            classifier_verdict: classifier_verdict.to_string(),
            classifier_confidence: None,
            classifier_detail: detail.to_string(),
        }
    }
}

/// Decision order (rustify `src/queue/track_check.rs`, extended with
/// the Phase 5 Gemini classifier):
///   1. disliked -> skip, reason disliked.
///   2. No Gemini key: wordlist profanity on title/artist first (the
///      old fast path — no lyrics fetch when the title itself is
///      profane).
///   3. Lyrics via LRCLib (real fetch; None = unavailable, never
///      faked), then the classifier: Gemini when a key is set (it
///      replaces the wordlist verdict), wordlist otherwise.
///   4. `ai_generated` skips only at confidence >= 0.7; below that
///      the verdict is reported but does not skip.
fn decide_skip(
    disliked_by: Option<i64>,
    _track_id: &str,
    title: &str,
    artist: &str,
) -> SkipDecision {
    // 1. Disliked by this user (or by anyone, when no user is given).
    if let Some(by) = disliked_by {
        let mut d = SkipDecision::clean("none", "unknown", "disliked short-circuits classification");
        d.skip = true;
        d.reason = Some("disliked".to_string());
        d.disliked_by = Some(by);
        return d;
    }

    // 2. Wordlist fast path when no Gemini key is configured.
    if !classifier::gemini_configured() {
        let text = format!("{} {}", title, artist);
        if let Some(word) = profanity_check(&text) {
            let mut d = SkipDecision::clean("wordlist", "profanity", "profanity in title/artist (wordlist)");
            d.skip = true;
            d.reason = Some("profanity".to_string());
            d.matched_word = Some(word);
            return d;
        }
    }

    // 3. Profanity/AI verdict over lyrics (Genius -> Musixmatch ->
    //    LRCLib, real fetch). Unreachable or no lyrics =>
    //    lyrics_available:false.
    let (lyrics, lyrics_source) = if title.is_empty() {
        (None, None)
    } else {
        let (lyrics, source) = fetch_lyrics(title, artist);
        (lyrics, source.map(|s| s.to_string()))
    };
    let lyrics_available = lyrics.is_some();
    let result = classifier::classify_track(title, artist, lyrics.as_deref(), &profanity_check);
    let mut d = SkipDecision {
        skip: false,
        reason: None,
        matched_word: None,
        disliked_by: None,
        lyrics_checked: lyrics_available,
        lyrics_available,
        lyrics_source,
        classifier_provider: result.provider.clone(),
        classifier_verdict: classifier::verdict_name(&result.verdict).to_string(),
        classifier_confidence: match &result.verdict {
            classifier::AiVerdict::AiGenerated { confidence } => Some(*confidence),
            _ => None,
        },
        classifier_detail: result.detail.clone(),
    };
    match &result.verdict {
        classifier::AiVerdict::Profanity { matched } => {
            d.skip = true;
            d.reason = Some("profanity".to_string());
            d.matched_word = Some(matched.clone());
        }
        classifier::AiVerdict::AiGenerated { confidence }
            if *confidence >= classifier::AI_SKIP_CONFIDENCE =>
        {
            d.skip = true;
            d.reason = Some("ai-generated".to_string());
        }
        _ => {}
    }
    d
}

/// Last polled currently-playing state for one watched user
/// (in-memory only — playback state is ephemeral; the watch registry
/// itself is persisted, this is not).
#[derive(Debug, Clone, Default)]
struct NowPlaying {
    track_id: String,
    title: String,
    artist: String,
    duration_sec: i64,
    is_playing: bool,
    checked_at: i64,
    last_skip: bool,
    last_reason: Option<String>,
    classifier_verdict: String,
}

/// skip-check decision order (rustify `src/queue/track_check.rs`):
/// disliked -> profanity in title/artist -> profanity in LRCLib lyrics.
/// `reason` is one of profanity | disliked | null. `ai-generated` is
/// never emitted: no classifier exists in this service (see README).
///
/// The decision itself lives in `decide_skip` (shared by the HTTP
/// handler and the poller); this handler only validates input and
/// renders the response.
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
    let disliked_by = st.disliked_by(&track_id, user_id);
    let d = decide_skip(disliked_by, &track_id, &title, &artist);
    (
        200,
        ok(&format!(
            "{{\"skip\":{},\"reason\":{},\"matched_word\":{},\"disliked_by\":{},\"lyrics_checked\":{},\"lyrics_available\":{},\"lyrics_source\":{},\"classifier_provider\":{},\"classifier_verdict\":{},\"classifier_confidence\":{},\"classifier_detail\":{}}}",
            d.skip,
            match &d.reason {
                Some(r) => quote(r),
                None => "null".to_string(),
            },
            match &d.matched_word {
                Some(w) => quote(w),
                None => "null".to_string(),
            },
            match d.disliked_by {
                Some(u) => u.to_string(),
                None => "null".to_string(),
            },
            d.lyrics_checked,
            d.lyrics_available,
            match &d.lyrics_source {
                Some(s) => quote(s),
                None => "null".to_string(),
            },
            quote(&d.classifier_provider),
            quote(&d.classifier_verdict),
            match d.classifier_confidence {
                Some(c) => format!("{}", c),
                None => "null".to_string(),
            },
            quote(&d.classifier_detail),
        )),
    )
}

/// IPC-7 (§29) `GET /v1/spotify/details?user_id=&track_id=` — the
/// details card the core renders for /details and the Spotify
/// companion views: the Track, audio features (null when the
/// deprecated endpoint refuses the app token), artist genres, how
/// many users disliked the track, the first profanity word in the
/// lyrics (if any), and the lyrics + Genius page link (when Genius
/// supplied them).
///
/// `user_id` is accepted (the core always sends the requesting user)
/// but not needed: `disliked_by` is the count across ALL users, not
/// a per-user flag. `ignored_by` is 0: no ignore store exists (see
/// port-engines-gaps.md — the §25 Hide-alerts button has no engine
/// endpoint yet). `language` is null: no language detection exists.
fn handle_details(st: &State, tokens: &TokenCache, track_id: &str) -> (u16, String) {
    if spotify_creds().is_none() {
        return (
            502,
            err(
                "engine_unavailable",
                "Spotify details need SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET (client-credentials flow)",
            ),
        );
    }
    let raw = match spotify_api_get(tokens, &format!("/tracks/{}", track_id)) {
        Some(v) => v,
        None => {
            return (
                502,
                err(
                    "engine_unavailable",
                    &format!("Spotify track lookup failed for {}", track_id),
                ),
            )
        }
    };
    let track = match spotify_track_to_json(&raw) {
        Some(t) => t,
        None => {
            return (
                502,
                err(
                    "engine_unavailable",
                    &format!("Spotify returned no usable track for {}", track_id),
                ),
            )
        }
    };
    let title = raw
        .get("name")
        .and_then(|x| x.as_str())
        .unwrap_or("")
        .to_string();
    let artist = match raw.get("artists") {
        Some(Json::Arr(items)) => items
            .iter()
            .filter_map(|a| a.get("name").and_then(|x| x.as_str()))
            .collect::<Vec<_>>()
            .join(", "),
        _ => String::new(),
    };
    // Genres from /artists?ids= — enrichment only: a failed call
    // leaves an empty list, it never fails the details response.
    let genres = match raw.get("artists") {
        Some(Json::Arr(items)) => {
            let ids: Vec<&str> = items
                .iter()
                .filter_map(|a| a.get("id").and_then(|x| x.as_str()))
                .collect();
            if ids.is_empty() {
                Vec::new()
            } else {
                match spotify_api_get(tokens, &format!("/artists?ids={}", ids.join(","))) {
                    Some(v) => artist_genres(&v),
                    None => Vec::new(),
                }
            }
        }
        _ => Vec::new(),
    };
    // Audio features: deprecated endpoint, likely 403 for app tokens.
    // Null when unavailable — never faked.
    let features = spotify_api_get(tokens, &format!("/audio-features/{}", track_id))
        .and_then(|v| audio_features_to_json(&v))
        .unwrap_or(Json::Null);
    let disliked_by = st
        .dislikes
        .values()
        .filter(|set| set.contains(track_id))
        .count();
    let (lyrics, _source, lyrics_link) = fetch_lyrics_with_url(&title, &artist);
    let profanity = lyrics.as_deref().and_then(profanity_check);
    let lyrics_json = match &lyrics {
        Some(l) => quote(l),
        None => "null".to_string(),
    };
    let lyrics_link_json = match &lyrics_link {
        Some(l) => quote(l),
        None => "null".to_string(),
    };
    let profanity_json = match &profanity {
        Some(p) => quote(p),
        None => "null".to_string(),
    };
    (
        200,
        ok(&format!(
            "{{\"track\":{},\"features\":{},\"genres\":[{}],\"disliked_by\":{},\"ignored_by\":0,\"profanity\":{},\"language\":null,\"lyrics\":{},\"lyrics_link\":{}}}",
            track.render(),
            features.render(),
            genres.iter().map(|g| quote(g)).collect::<Vec<_>>().join(","),
            disliked_by,
            profanity_json,
            lyrics_json,
            lyrics_link_json
        )),
    )
}

/// IPC-7 (§29) `POST /v1/spotify/analyze` {user_id, track_id} — the
/// "Analyze this song" companion action: fetch the lyrics (404
/// `not_found` when there are none, matching the inventory's "🚫
/// Lyrics not found"), list EVERY profanity word in them (deduped,
/// in order), and run the classifier for the one-sentence analysis.
fn handle_analyze(tokens: &TokenCache, body: &Json) -> (u16, String) {
    let track_id = match body.get("track_id").and_then(|v| v.as_str()) {
        Some(t) if !t.is_empty() => t.to_string(),
        _ => {
            return (
                400,
                err("bad_request", "missing string field: track_id"),
            )
        }
    };
    if spotify_creds().is_none() {
        return (
            502,
            err(
                "engine_unavailable",
                "Spotify analyze needs SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET (client-credentials flow)",
            ),
        );
    }
    let raw = match spotify_api_get(tokens, &format!("/tracks/{}", track_id)) {
        Some(v) => v,
        None => {
            return (
                502,
                err(
                    "engine_unavailable",
                    &format!("Spotify track lookup failed for {}", track_id),
                ),
            )
        }
    };
    let title = raw
        .get("name")
        .and_then(|x| x.as_str())
        .unwrap_or("")
        .to_string();
    let artist = match raw.get("artists") {
        Some(Json::Arr(items)) => items
            .iter()
            .filter_map(|a| a.get("name").and_then(|x| x.as_str()))
            .collect::<Vec<_>>()
            .join(", "),
        _ => String::new(),
    };
    let (lyrics, _source) = fetch_lyrics(&title, &artist);
    let lyrics = match lyrics {
        Some(l) => l,
        None => {
            return (
                404,
                err("not_found", "no lyrics found for this track"),
            )
        }
    };
    let words = profanity_words(&lyrics);
    let result = classifier::classify_track(&title, &artist, Some(&lyrics), &profanity_check);
    (
        200,
        ok(&format!(
            "{{\"analysis\":{},\"profane_words\":[{}]}}",
            quote(&analysis_text(&result)),
            words.iter().map(|w| quote(w)).collect::<Vec<_>>().join(",")
        )),
    )
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
fn handle_status(
    tokens: &TokenCache,
    user_tokens: usize,
    watching: usize,
    poll_interval_secs: u64,
) -> (u16, String) {
    let configured = spotify_creds().is_some();
    let cached = {
        let guard = tokens.lock().unwrap_or_else(|e| e.into_inner());
        token_still_valid(&guard, now_unix())
    };
    (
        200,
        ok(&format!(
            "{{\"credentials_configured\":{},\"token_cached\":{},\"api\":\"spotify-web-api\",\"user_tokens\":{},\"watching\":{},\"gemini_configured\":{},\"poll_interval_secs\":{},\"polling\":{}}}",
            configured,
            cached,
            user_tokens,
            watching,
            classifier::gemini_configured(),
            poll_interval_secs,
            poll_interval_secs > 0
        )),
    )
}

/// `POST /v1/spotify/auth` — per-user OAuth (rustify
/// `src/spotify/auth.rs` + `Manager::for_user`).
///   {action:"login", user_id}    -> {authorize_url} (user completes
///                                    it in a browser; the code comes
///                                    back via "callback")
///   {action:"callback", user_id, code} -> exchanges the code, stores
///                                    the encrypted tokens
///   {action:"refresh", user_id}  -> refreshes the stored token
///   {action:"revoke", user_id}   -> deletes the stored token
fn handle_auth(
    tokens: &mut auth::TokenStore,
    data_dir: &std::path::Path,
    body: &Json,
) -> (u16, String) {
    let action = match body.get("action").and_then(|v| v.as_str()) {
        Some(a) => a.to_string(),
        None => return (400, err("bad_request", "missing string field: action")),
    };
    let user_id = match need_user(body) {
        Ok(u) => u,
        Err(e) => return e,
    };
    let (client_id, secret, redirect) = match auth::oauth_client() {
        Some(c) => c,
        None => {
            return (
                503,
                err(
                    "engine_unavailable",
                    "per-user Spotify auth needs SPOTIFY_CLIENT_ID and SPOTIFY_CLIENT_SECRET (and SPOTIFY_REDIRECT_URI for login)",
                ),
            )
        }
    };
    match action.as_str() {
        "login" => {
            let state = format!("{}-{}", user_id, crate::common::random_token(16));
            let url = auth::authorize_url(&client_id, &redirect, &state);
            (
                200,
                ok(&format!(
                    "{{\"authorized\":false,\"user_id\":{},\"authorize_url\":{},\"note\":\"complete the URL in a browser, then POST action:callback with the returned code\"}}",
                    user_id,
                    quote(&url)
                )),
            )
        }
        "callback" => {
            let code = match body.get("code").and_then(|v| v.as_str()) {
                Some(c) if !c.is_empty() => c.to_string(),
                _ => return (400, err("bad_request", "missing string field: code")),
            };
            match auth::exchange_code(
                &auth::accounts_base(),
                &client_id,
                &secret,
                &redirect,
                &code,
                now_unix(),
            ) {
                Some(tok) => {
                    let expires_at = tok.expires_at;
                    tokens.set(user_id, tok);
                    tokens.save(data_dir);
                    (
                        200,
                        ok(&format!(
                            "{{\"authorized\":true,\"user_id\":{},\"expires_at\":{},\"note\":\"token stored encrypted at rest\"}}",
                            user_id, expires_at
                        )),
                    )
                }
                None => (
                    502,
                    err(
                        "download_failed",
                        "authorization code exchange failed (bad/expired code, or Spotify rejected the client)",
                    ),
                ),
            }
        }
        "refresh" => {
            let old = match tokens.get(user_id) {
                Some(t) => t.clone(),
                None => {
                    return (
                        404,
                        err(
                            "not_found",
                            "no stored token for this user (login first via action:login)",
                        ),
                    )
                }
            };
            match auth::refresh_token(&auth::accounts_base(), &client_id, &secret, &old, now_unix())
            {
                Some(tok) => {
                    let expires_at = tok.expires_at;
                    tokens.set(user_id, tok);
                    tokens.save(data_dir);
                    (
                        200,
                        ok(&format!(
                            "{{\"authorized\":true,\"user_id\":{},\"expires_at\":{}}}",
                            user_id, expires_at
                        )),
                    )
                }
                None => {
                    // Refresh token dead (revoked/expired): drop it so
                    // the user is prompted to log in again instead of
                    // retrying forever (rustify token_refresh).
                    tokens.remove(user_id);
                    tokens.save(data_dir);
                    (
                        401,
                        err(
                            "unauthorized",
                            "refresh token rejected by Spotify (invalid_grant); token dropped, login again via action:login",
                        ),
                    )
                }
            }
        }
        "revoke" => {
            let was = tokens.remove(user_id);
            tokens.save(data_dir);
            (
                200,
                ok(&format!(
                    "{{\"authorized\":false,\"user_id\":{},\"was_authorized\":{}}}",
                    user_id, was
                )),
            )
        }
        _ => (
            400,
            err("bad_request", "unknown action (login|callback|refresh|revoke)"),
        ),
    }
}

/// `GET /v1/spotify/now-playing?user_id=N` — last polled
/// currently-playing track for a watched user (from the poller, not
/// a live fetch: the poller is the only thing that talks to Spotify
/// on a schedule).
fn handle_now_playing(
    now_playing: &BTreeMap<i64, NowPlaying>,
    user_id: i64,
) -> (u16, String) {
    match now_playing.get(&user_id) {
        Some(np) if !np.track_id.is_empty() => (
            200,
            ok(&format!(
                "{{\"playing\":{},\"user_id\":{},\"track\":{{\"id\":{},\"title\":{},\"artist\":{},\"duration_sec\":{}}},\"checked_at\":{},\"last_skip\":{},\"last_reason\":{},\"classifier_verdict\":{}}}",
                np.is_playing,
                user_id,
                quote(&np.track_id),
                quote(&np.title),
                quote(&np.artist),
                np.duration_sec,
                np.checked_at,
                np.last_skip,
                match &np.last_reason {
                    Some(r) => quote(r),
                    None => "null".to_string(),
                },
                quote(&np.classifier_verdict),
            )),
        ),
        _ => (
            200,
            ok(&format!(
                "{{\"playing\":false,\"user_id\":{},\"track\":null,\"note\":\"no currently-playing track polled for this user (not watched, no token, or nothing playing)\"}}",
                user_id
            )),
        ),
    }
}

/// Shared service state, cloned into the HTTP handler and the poller.
struct App {
    state: Arc<Mutex<State>>,
    tokens: TokenCache,
    user_tokens: Arc<Mutex<auth::TokenStore>>,
    now_playing: Arc<Mutex<BTreeMap<i64, NowPlaying>>>,
    data_dir: std::path::PathBuf,
    poll_interval_secs: u64,
}

fn handle(app: &App, req: &Request) -> (u16, String) {
    if req.method == "GET" && req.path == "/v1/spotify/watches" {
        let st = lock(&app.state);
        let ids: Vec<String> = st.watches.keys().map(|u| u.to_string()).collect();
        return (
            200,
            ok(&format!("{{\"watches\":[{}]}}", ids.join(","))),
        );
    }
    if req.method == "GET" && req.path == "/v1/spotify/status" {
        let (user_tokens, watching) = {
            let ut = app.user_tokens.lock().unwrap_or_else(|e| e.into_inner());
            let st = lock(&app.state);
            (ut.len(), st.watches.len())
        };
        return handle_status(&app.tokens, user_tokens, watching, app.poll_interval_secs);
    }
    if req.method == "GET" && req.path == "/v1/spotify/now-playing" {
        let user_id = match req.query_param("user_id").and_then(|v| v.parse::<i64>().ok()) {
            Some(u) => u,
            None => {
                return (
                    400,
                    err("bad_request", "missing integer query param: user_id"),
                )
            }
        };
        let np = app.now_playing.lock().unwrap_or_else(|e| e.into_inner());
        return handle_now_playing(&np, user_id);
    }
    if req.method == "GET" && req.path == "/v1/spotify/details" {
        // IPC-7 (§29): track_id required; user_id accepted but not
        // needed (disliked_by is a cross-user count).
        let track_id = match req.query_param("track_id") {
            Some(t) if !t.is_empty() => t,
            _ => {
                return (
                    400,
                    err("bad_request", "missing query param: track_id"),
                )
            }
        };
        let st = lock(&app.state);
        return handle_details(&st, &app.tokens, &track_id);
    }
    if req.method != "POST" {
        return (404, err("not_found", "route not found"));
    }
    let body = match parse_json(&req.body) {
        Some(b @ Json::Obj(_)) => b,
        _ => return (400, err("bad_request", "invalid JSON body")),
    };
    // Auth mutates the token store, not the watch registry: handle it
    // before taking the State lock.
    if req.path == "/v1/spotify/auth" {
        let mut ut = app.user_tokens.lock().unwrap_or_else(|e| e.into_inner());
        return handle_auth(&mut ut, &app.data_dir, &body);
    }
    let mut st = lock(&app.state);
    let res = match req.path.as_str() {
        "/v1/spotify/watch" => handle_watch(&mut st, &body),
        "/v1/spotify/dislike" => handle_dislike(&mut st, &body),
        "/v1/spotify/resolve" => handle_resolve(&st, &app.tokens, &body),
        "/v1/spotify/collection" => handle_collection(&app.tokens, &body),
        "/v1/spotify/skip-check" => handle_skip_check(&st, &body),
        "/v1/spotify/analyze" => handle_analyze(&app.tokens, &body),
        _ => return (404, err("not_found", "route not found")),
    };
    // skip-check and resolve are read-only; saving anyway is harmless
    // and keeps watch/dislike mutations durable on every write path.
    save_state(&st);
    res
}

/// Currently-playing poller (rustify `src/tick/user.rs::check`):
/// for each watched user that has a stored OAuth token, refresh the
/// token when it is near expiry, fetch `/v1/me/player/currently-
/// playing`, and — when the track changed — run the same skip-check
/// decision the HTTP handler runs and record the verdict. A 401
/// drops the token (user must log in again); any other failure is
/// logged and the previous state is kept.
///
/// This is what moves `spotify.watch` from intent-only to real
/// polling. It does NOT skip the track on the user's Spotify player:
/// Parlay never requests playback-modification scopes, so the
/// verdict is recorded (and visible via `/v1/spotify/now-playing`)
/// for the VC engine / core to act on.
fn poller_loop(app: Arc<App>) {
    let interval = std::time::Duration::from_secs(app.poll_interval_secs);
    loop {
        std::thread::sleep(interval);
        let watched: Vec<i64> = {
            let st = lock(&app.state);
            st.watches.keys().copied().collect()
        };
        for user_id in watched {
            // Snapshot the token (clone) so no lock is held during
            // network I/O.
            let tok = {
                let ut = app.user_tokens.lock().unwrap_or_else(|e| e.into_inner());
                ut.get(user_id).cloned()
            };
            let mut tok = match tok {
                Some(t) => t,
                None => continue, // watched but never logged in: intent only
            };
            // Refresh when near expiry.
            if tok.needs_refresh(now_unix()) {
                let (client_id, secret, _) = match auth::oauth_client() {
                    Some(c) => c,
                    None => continue,
                };
                match auth::refresh_token(
                    &auth::accounts_base(),
                    &client_id,
                    &secret,
                    &tok,
                    now_unix(),
                ) {
                    Some(new_tok) => {
                        tok = new_tok.clone();
                        let mut ut = app.user_tokens.lock().unwrap_or_else(|e| e.into_inner());
                        ut.set(user_id, new_tok);
                        ut.save(&app.data_dir);
                    }
                    None => {
                        eprintln!(
                            "[spotify-rust] poll: dropping dead token for user {} (refresh rejected)",
                            user_id
                        );
                        let mut ut = app.user_tokens.lock().unwrap_or_else(|e| e.into_inner());
                        ut.remove(user_id);
                        ut.save(&app.data_dir);
                        continue;
                    }
                }
            }
            let np = match auth::currently_playing(&auth::api_base(), &tok.access_token) {
                Ok(v) => v,
                Err(e) => {
                    if e.contains("unauthorized") {
                        eprintln!(
                            "[spotify-rust] poll: dropping unauthorized token for user {}",
                            user_id
                        );
                        let mut ut = app.user_tokens.lock().unwrap_or_else(|e| e.into_inner());
                        ut.remove(user_id);
                        ut.save(&app.data_dir);
                    } else {
                        eprintln!("[spotify-rust] poll: user {}: {}", user_id, e);
                    }
                    continue;
                }
            };
            let mut map = app.now_playing.lock().unwrap_or_else(|e| e.into_inner());
            let entry = map.entry(user_id).or_default();
            match np {
                None => {
                    entry.is_playing = false;
                    entry.checked_at = now_unix();
                }
                Some(track) => {
                    let changed = entry.track_id != track.id;
                    entry.track_id = track.id.clone();
                    entry.title = track.title.clone();
                    entry.artist = track.artist.clone();
                    entry.duration_sec = track.duration_sec;
                    entry.is_playing = track.is_playing;
                    entry.checked_at = now_unix();
                    if changed && track.is_playing {
                        // Same decision as the HTTP skip-check; the
                        // disliked lookup needs the State lock briefly.
                        let disliked_by = {
                            let st = lock(&app.state);
                            st.disliked_by(&track.id, Some(user_id))
                        };
                        let d = decide_skip(disliked_by, &track.id, &track.title, &track.artist);
                        entry.last_skip = d.skip;
                        entry.last_reason = d.reason.clone();
                        entry.classifier_verdict = d.classifier_verdict.clone();
                        if d.skip {
                            eprintln!(
                                "[spotify-rust] poll: user {} now playing {} — {} (skip:{}, reason:{:?})",
                                user_id, track.title, track.artist, d.skip, d.reason
                            );
                        }
                    }
                }
            }
        }
    }
}

fn main() {
    let port: u16 = std::env::var("PORT")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(8007);
    let data_dir = std::path::PathBuf::from(
        std::env::var("PARLAY_DATA_DIR").unwrap_or_else(|_| "data".to_string()),
    );
    let _ = std::fs::create_dir_all(&data_dir);
    // Shared durable store (A11): Mongo > Redis > JSON file. A
    // configured-but-unreachable backend is fatal — see store.rs.
    if let Err(e) = store::init() {
        eprintln!("[spotify-rust] shared store init failed: {}", e);
        std::process::exit(1);
    }
    let poll_interval_secs: u64 = std::env::var("SPOTIFY_POLL_INTERVAL_SECS")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(30);
    let app = Arc::new(App {
        state: Arc::new(Mutex::new(load_state())),
        // App-token cache lives outside the persisted State: the access
        // token is a secret and is never written to disk.
        tokens: Arc::new(Mutex::new(None)),
        user_tokens: Arc::new(Mutex::new(auth::TokenStore::load(&data_dir))),
        now_playing: Arc::new(Mutex::new(BTreeMap::new())),
        data_dir,
        poll_interval_secs,
    });
    if poll_interval_secs > 0 {
        let poller_app = Arc::clone(&app);
        std::thread::spawn(move || poller_loop(poller_app));
        eprintln!(
            "[spotify-rust] currently-playing poller running every {}s",
            poll_interval_secs
        );
    } else {
        eprintln!("[spotify-rust] poller disabled (SPOTIFY_POLL_INTERVAL_SECS=0)");
    }
    let handler_app = Arc::clone(&app);
    let handler = move |req: &Request| handle(&handler_app, req);
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

    // ------------------------------------------- Genius / Musixmatch ---

    const GENIUS_SEARCH_FIXTURE: &str = r#"{
        "response": {"hits": [
            {"result": {"url": "https://genius.com/Cover-Artist-Wrong-Song-lyrics",
                        "title": "Wrong Song",
                        "primary_artist": {"name": "Cover Artist"}}},
            {"result": {"url": "https://genius.com/Rick-Astley-Never-Gonna-Give-You-Up-lyrics",
                        "title": "Never Gonna Give You Up",
                        "primary_artist": {"name": "Rick Astley"}}}
        ]}
    }"#;

    #[test]
    fn genius_hit_prefers_matching_artist() {
        let v = parse_json(GENIUS_SEARCH_FIXTURE).expect("fixture parses");
        assert_eq!(
            pick_genius_hit(&v, "Rick Astley"),
            Some(
                "https://genius.com/Rick-Astley-Never-Gonna-Give-You-Up-lyrics"
                    .to_string()
            )
        );
    }

    #[test]
    fn genius_hit_falls_back_to_first_hit() {
        let v = parse_json(GENIUS_SEARCH_FIXTURE).expect("fixture parses");
        assert_eq!(
            pick_genius_hit(&v, "Nobody In Particular"),
            Some("https://genius.com/Cover-Artist-Wrong-Song-lyrics".to_string())
        );
    }

    #[test]
    fn genius_hit_none_on_empty_response() {
        let v = parse_json(r#"{"response": {"hits": []}}"#).expect("parses");
        assert_eq!(pick_genius_hit(&v, "Rick Astley"), None);
        let v2 = parse_json(r#"{}"#).expect("parses");
        assert_eq!(pick_genius_hit(&v2, "Rick Astley"), None);
    }

    const GENIUS_PAGE_FIXTURE: &str = r#"<html><body>
        <div class="noise">not lyrics</div>
        <div data-lyrics-container="true" class="Lyrics__Container-sc-1ynbvzw-1">
            [Verse 1]<br>We're no strangers to love<br>
            You know the rules &amp; so do I
            <div class="nested">A <a href="/x">nested</a> line</div>
            <span>Don't tell me you're too blind to see</span>
        </div>
        <div data-lyrics-container="true">[Chorus]<br>Never gonna give you up</div>
    </body></html>"#;

    #[test]
    fn genius_page_extraction() {
        let lyrics = extract_genius_lyrics_html(GENIUS_PAGE_FIXTURE)
            .expect("containers present");
        assert!(lyrics.contains("[Verse 1]"), "lyrics:\n{}", lyrics);
        assert!(lyrics.contains("We're no strangers to love"), "lyrics:\n{}", lyrics);
        assert!(
            lyrics.contains("You know the rules & so do I"),
            "entity unescaped: {}", lyrics
        );
        assert!(lyrics.contains("A nested line"), "nested div: {}", lyrics);
        assert!(lyrics.contains("[Chorus]"), "second container: {}", lyrics);
        assert!(lyrics.contains("Never gonna give you up"), "lyrics:\n{}", lyrics);
        assert!(!lyrics.contains("not lyrics"), "noise excluded");
        assert!(!lyrics.contains("<div"), "tags stripped");
    }

    #[test]
    fn genius_page_none_without_container() {
        assert_eq!(extract_genius_lyrics_html("<html><body>no lyrics here</body></html>"), None);
        assert_eq!(
            extract_genius_lyrics_html(r#"<div data-lyrics-container="true"></div>"#),
            None
        );
    }

    #[test]
    fn html_tag_strip_and_entities() {
        assert_eq!(strip_html_tags("<p>hello <b>world</b></p>"), "hello world");
        assert_eq!(strip_html_tags("a<!-- c -->b"), "ab");
        assert_eq!(
            unescape_html_entities("a &amp; b &lt;c&gt; &quot;q&quot; &#39;x&#39; &nbsp;end"),
            "a & b <c> \"q\" 'x'  end"
        );
    }

    const MUSIXMATCH_OK_FIXTURE: &str = r#"{
        "message": {"header": {"status_code": 200},
                    "body": {"lyrics": {"lyrics_body": "Never gonna give you up\nNever gonna let you down",
                                        "restricted": 0, "instrumental": 0}}}
    }"#;

    #[test]
    fn musixmatch_parse_ok() {
        let v = parse_json(MUSIXMATCH_OK_FIXTURE).expect("fixture parses");
        let lyrics = parse_musixmatch_lyrics(&v).expect("200 + body");
        assert!(lyrics.contains("Never gonna give you up"), "{}", lyrics);
    }

    #[test]
    fn musixmatch_parse_rejects_restricted_instrumental_error() {
        let restricted = parse_json(
            r#"{"message": {"header": {"status_code": 200},
               "body": {"lyrics": {"lyrics_body": "partial…", "restricted": 1, "instrumental": 0}}}}"#,
        )
        .expect("parses");
        assert_eq!(parse_musixmatch_lyrics(&restricted), None, "restricted -> None");
        let instrumental = parse_json(
            r#"{"message": {"header": {"status_code": 200},
               "body": {"lyrics": {"lyrics_body": "", "restricted": 0, "instrumental": 1}}}}"#,
        )
        .expect("parses");
        assert_eq!(parse_musixmatch_lyrics(&instrumental), None, "instrumental -> None");
        let not_found = parse_json(
            r#"{"message": {"header": {"status_code": 404}, "body": {}}}"#,
        )
        .expect("parses");
        assert_eq!(parse_musixmatch_lyrics(&not_found), None, "404 -> None");
        let empty = parse_json(
            r#"{"message": {"header": {"status_code": 200},
               "body": {"lyrics": {"lyrics_body": "   ", "restricted": 0, "instrumental": 0}}}}"#,
        )
        .expect("parses");
        assert_eq!(parse_musixmatch_lyrics(&empty), None, "empty body -> None");
    }

    /// Env-gating: with no keys in the environment both credentialed
    /// providers return None immediately (no network attempted). Skips
    /// honestly if a key happens to be set in the test environment.
    #[test]
    fn lyrics_providers_none_without_keys() {
        if std::env::var("GENIUS_ACCESS_TOKEN")
            .map(|v| !v.trim().is_empty())
            .unwrap_or(false)
        {
            eprintln!("skipping Genius no-key test (GENIUS_ACCESS_TOKEN set)");
        } else {
            assert_eq!(fetch_genius_lyrics("Never Gonna Give You Up", "Rick Astley"), None);
            assert!(!genius_configured());
        }
        if std::env::var("MUSIXMATCH_API_KEY")
            .map(|v| !v.trim().is_empty())
            .unwrap_or(false)
        {
            eprintln!("skipping Musixmatch no-key test (MUSIXMATCH_API_KEY set)");
        } else {
            assert_eq!(fetch_musixmatch_lyrics("Never Gonna Give You Up", "Rick Astley"), None);
            assert!(!musixmatch_configured());
        }
    }

    // ------------------------------------------------ Phase 9: IPC-7 --

    #[test]
    fn format_duration_str_cases() {
        // Same rendering as vc-go / vc-ts / vc-rust / downloaders /
        // netease-go (asserted identically in each service's tests).
        assert_eq!(format_duration_str(0), "∞");
        assert_eq!(format_duration_str(-5), "∞");
        assert_eq!(format_duration_str(45), "0:45");
        assert_eq!(format_duration_str(268), "4:28");
        assert_eq!(format_duration_str(3599), "59:59");
        assert_eq!(format_duration_str(3600), "1h");
        assert_eq!(format_duration_str(3661), "1h 1m 1s");
        assert_eq!(format_duration_str(3720), "1h 2m");
        assert_eq!(format_duration_str(3605), "1h 5s");
    }

    #[test]
    fn track_mapping_carries_duration_str() {
        let v = parse_json(SAMPLE_TRACK_JSON).expect("fixture parses");
        let t = spotify_track_to_json(&v).expect("mapping");
        assert_eq!(
            t.get("duration_str").and_then(|x| x.as_str()),
            Some("3:33")
        );
        assert_eq!(t.get("duration_sec").and_then(|x| x.as_i64()), Some(213));
    }

    #[test]
    fn profanity_words_collects_all_deduped_in_order() {
        assert_eq!(
            profanity_words("shit and fuck and shit again, bitch"),
            vec!["shit".to_string(), "fuck".to_string(), "bitch".to_string()]
        );
        assert_eq!(
            profanity_words("clean lyrics here"),
            Vec::<String>::new()
        );
        // Case-insensitive, word-boundary: "classic" is not "ass".
        assert_eq!(profanity_words("classic"), Vec::<String>::new());
        // profanity_check still returns only the first match.
        assert_eq!(
            profanity_check("bitch then shit"),
            Some("bitch".to_string())
        );
    }

    #[test]
    fn audio_features_mapping() {
        let v = parse_json(
            r#"{"key": 9, "mode": 0, "tempo": 120.5, "acousticness": 0.01,
                "danceability": 0.75, "energy": 0.9, "instrumentalness": 0.0,
                "liveness": 0.1, "speechiness": 0.05, "valence": 0.8}"#,
        )
        .expect("parses");
        let f = audio_features_to_json(&v).expect("mapping");
        assert_eq!(f.get("key").and_then(|x| x.as_str()), Some("A"));
        assert_eq!(f.get("modality").and_then(|x| x.as_str()), Some("Minor"));
        assert_eq!(f.get("tempo_bpm").and_then(|x| x.as_f64()), Some(120.5));
        assert_eq!(f.get("danceability").and_then(|x| x.as_f64()), Some(0.75));
        assert_eq!(f.get("valence").and_then(|x| x.as_f64()), Some(0.8));
        // Key -1 (no key detected) renders as Unknown, not a fake note.
        let unknown = parse_json(r#"{"key": -1, "mode": 1, "tempo": 100}"#).unwrap();
        let f = audio_features_to_json(&unknown).expect("mapping");
        assert_eq!(f.get("key").and_then(|x| x.as_str()), Some("Unknown"));
        assert_eq!(f.get("modality").and_then(|x| x.as_str()), Some("Major"));
        // Not an object -> None (the caller renders features: null).
        assert_eq!(audio_features_to_json(&Json::Null), None);
        assert_eq!(audio_features_to_json(&Json::Str("nope".into())), None);
    }

    #[test]
    fn artist_genres_dedups_in_order() {
        let v = parse_json(
            r#"{"artists": [{"genres": ["pop", "dance pop"]},
                            {"genres": ["dance pop", "rock"]},
                            {"genres": []}]}"#,
        )
        .expect("parses");
        assert_eq!(
            artist_genres(&v),
            vec!["pop".to_string(), "dance pop".to_string(), "rock".to_string()]
        );
        assert_eq!(artist_genres(&Json::Null), Vec::<String>::new());
    }

    /// The wordlist classifier path is deterministic without keys or
    /// network; skip honestly when a Gemini key is set (the live path
    /// would call the API).
    #[test]
    fn analysis_text_wordlist_paths() {
        if classifier::gemini_configured() {
            eprintln!("skipping analysis_text test (GEMINI_API_KEY set)");
            return;
        }
        let clean = classifier::classify_track(
            "Never Gonna Give You Up", "Rick Astley",
            Some("never gonna give you up, never gonna let you down"),
            &profanity_check,
        );
        assert_eq!(
            analysis_text(&clean),
            "Verdict: clean (wordlist) — no profanity found (wordlist)"
        );
        let profane = classifier::classify_track(
            "Angry Song", "Someone",
            Some("this shit is fucking awful"),
            &profanity_check,
        );
        assert_eq!(
            analysis_text(&profane),
            "Verdict: profanity (wordlist) — profanity in lyrics (wordlist)"
        );
    }

    fn test_app() -> App {
        App {
            state: Arc::new(Mutex::new(State::default())),
            tokens: Arc::new(Mutex::new(None)),
            user_tokens: Arc::new(Mutex::new(auth::TokenStore::load(
                &std::env::temp_dir(),
            ))),
            now_playing: Arc::new(Mutex::new(BTreeMap::new())),
            data_dir: std::env::temp_dir(),
            poll_interval_secs: 60,
        }
    }

    fn test_req(method: &str, path: &str, query: &str, body: &str) -> Request {
        Request {
            method: method.to_string(),
            path: path.to_string(),
            query: query.to_string(),
            headers: Vec::new(),
            body: body.to_string(),
        }
    }

    /// Handler tests without Spotify credentials are deterministic
    /// (they fail before any network call). Skip honestly when creds
    /// happen to be set in the test environment.
    #[test]
    fn details_without_creds_is_502() {
        if spotify_creds().is_some() {
            eprintln!("skipping details no-creds test (Spotify creds set)");
            return;
        }
        let app = test_app();
        let (st, resp) = handle(
            &app,
            &test_req("GET", "/v1/spotify/details",
                      "user_id=123&track_id=4uLU6hMCjMI75M1A2tKUQC", ""),
        );
        assert_eq!(st, 502, "{}", resp);
        assert!(resp.contains("engine_unavailable"), "{}", resp);
        assert!(resp.contains("SPOTIFY_CLIENT_ID"), "{}", resp);
    }

    #[test]
    fn details_requires_track_id() {
        let app = test_app();
        let (st, resp) = handle(
            &app,
            &test_req("GET", "/v1/spotify/details", "user_id=123", ""),
        );
        assert_eq!(st, 400, "{}", resp);
        assert!(resp.contains("track_id"), "{}", resp);
    }

    #[test]
    fn analyze_without_creds_is_502() {
        if spotify_creds().is_some() {
            eprintln!("skipping analyze no-creds test (Spotify creds set)");
            return;
        }
        let app = test_app();
        let (st, resp) = handle(
            &app,
            &test_req("POST", "/v1/spotify/analyze", "",
                      r#"{"user_id":123,"track_id":"4uLU6hMCjMI75M1A2tKUQC"}"#),
        );
        assert_eq!(st, 502, "{}", resp);
        assert!(resp.contains("engine_unavailable"), "{}", resp);
    }

    #[test]
    fn analyze_requires_track_id() {
        let app = test_app();
        let (st, resp) = handle(
            &app,
            &test_req("POST", "/v1/spotify/analyze", "", r#"{"user_id":123}"#),
        );
        assert_eq!(st, 400, "{}", resp);
        assert!(resp.contains("track_id"), "{}", resp);
    }
}
