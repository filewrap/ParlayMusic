//! Per-user Spotify OAuth tokens, encrypted at rest (Phase 5, port of
//! rustify `src/spotify/auth.rs` — `SpotifyAuthService::set_token` /
//! `get_token` — re-implemented on Parlay's std-only stack).
//!
//! Rustify stores tokens in SeaORM (Postgres); Parlay stores them in a
//! JSON file under the service data dir, each record encrypted with
//! AES-256-GCM (see `crypto.rs`) under a single service key:
//!
//!   * Key file `spotify-user-key` — 32 random bytes, hex, mode 0600,
//!     generated once at first run from /dev/urandom. There is NO
//!     hardcoded default key anywhere: a fresh checkout generates a
//!     fresh key, and tokens encrypted under an old key are simply
//!     unreadable (they are skipped with a warning, never decrypted
//!     with a fallback).
//!   * Token file `spotify-user-tokens.json` — one record per user:
//!     `{user_id, nonce (hex, 12 bytes), ciphertext (hex), expires_at}`.
//!     Plaintext is `{"access_token":...,"refresh_token":...}`.
//!     AAD is `parlay:spotify-user-token:v1:<user_id>`, so a record
//!     cannot be transplanted to another user id.
//!   * The access token is NEVER written to disk in plaintext, and the
//!     app-only client-credentials token cache in main.rs stays
//!     in-memory only, as before.
//!
//! The same encrypted-store pattern (one key file + one JSON record
//! file, AES-256-GCM, per-record nonce, AAD bound to the owner id) is
//! the documented pattern for ANY other per-user credential this
//! service — or any Parlay service — needs to persist; copy this
//! module, change the AAD tag and the file names, keep the rest.
//!
//! OAuth flow (rustify `Manager::new` / `for_user`):
//!   * `POST /v1/spotify/auth {action:"login", user_id}` returns the
//!     Spotify authorize URL (scopes: user-read-currently-playing,
//!     user-read-playback-state). The user completes it in a browser
//!     and the code comes back to `action:"callback"`.
//!   * `action:"callback" {user_id, code}` exchanges the code at
//!     accounts.spotify.com/api/token and stores the encrypted tokens.
//!   * `action:"refresh" {user_id}` refreshes an expiring token.
//!   * `action:"revoke" {user_id}` deletes the stored token.
//!   * The poller (main.rs) refreshes automatically when a token is
//!     within 120 s of expiry, exactly like rustify's
//!     `Manager::token_refresh`; a refresh that fails with
//!     invalid_grant drops the token (the user must /login again)
//!     instead of retrying forever.
//!
//! Testability: the accounts/API base URLs are parameters
//! (`SPOTIFY_ACCOUNTS_URL`, `SPOTIFY_API_BASE` env, defaulting to the
//! real Spotify hosts), so tests point them at a local stub server —
//! no real key, no network to Spotify.

use crate::common::*;
use crate::crypto;
use std::collections::BTreeMap;
use std::process::Command;

const KEY_FILE: &str = "spotify-user-key";
const TOKENS_FILE: &str = "spotify-user-tokens.json";
const AAD_PREFIX: &str = "parlay:spotify-user-token:v1:";

/// Scopes the service needs (rustify `Manager::new`, reduced to the
/// minimum for watch + skip-check; playback modification is NOT
/// requested — Parlay never controls the user's Spotify player).
pub const OAUTH_SCOPES: &str = "user-read-currently-playing user-read-playback-state";

#[derive(Debug, Clone)]
pub struct UserToken {
    pub access_token: String,
    pub refresh_token: String,
    pub expires_at: i64,
}

impl UserToken {
    pub fn needs_refresh(&self, now: i64) -> bool {
        self.expires_at <= now + 120
    }
}

fn aad_for(user_id: i64) -> String {
    format!("{}{}", AAD_PREFIX, user_id)
}

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

/// The encrypted per-user token store. Plaintext tokens live in
/// memory only while the service runs; on disk they are always
/// AES-256-GCM ciphertext (see the module doc comment).
pub struct TokenStore {
    key: [u8; 32],
    tokens: BTreeMap<i64, UserToken>,
}

impl TokenStore {
    /// Load (or create) the store under `data_dir`. A missing key file
    /// generates a fresh key; a missing token file starts empty. A
    /// record that fails to decrypt is skipped with a warning — a
    /// rotated/lost key must never crash the service or leak a token.
    pub fn load(data_dir: &std::path::Path) -> TokenStore {
        let key_path = data_dir.join(KEY_FILE);
        let key = match std::fs::read_to_string(&key_path) {
            Ok(hex) => match crypto::from_hex(hex.trim()) {
                Some(bytes) if bytes.len() == 32 => {
                    let mut k = [0u8; 32];
                    k.copy_from_slice(&bytes);
                    k
                }
                _ => {
                    eprintln!(
                        "[spotify-rust] WARNING: key file {} is corrupt; generating a fresh key (old tokens unreadable)",
                        key_path.display()
                    );
                    Self::generate_and_store_key(&key_path)
                }
            },
            Err(_) => Self::generate_and_store_key(&key_path),
        };
        let mut store = TokenStore {
            key,
            tokens: BTreeMap::new(),
        };
        let tokens_path = data_dir.join(TOKENS_FILE);
        if let Ok(text) = std::fs::read_to_string(&tokens_path) {
            if let Some(v) = parse_json(&text) {
                if let Some(Json::Arr(records)) = v.get("users") {
                    for rec in records {
                        let user_id = match rec.get("user_id").and_then(|x| x.as_i64()) {
                            Some(u) => u,
                            None => continue,
                        };
                        let nonce = match rec
                            .get("nonce")
                            .and_then(|x| x.as_str())
                            .and_then(crypto::from_hex)
                        {
                            Some(n) if n.len() == 12 => {
                                let mut a = [0u8; 12];
                                a.copy_from_slice(&n);
                                a
                            }
                            _ => continue,
                        };
                        let ct = match rec
                            .get("ciphertext")
                            .and_then(|x| x.as_str())
                            .and_then(crypto::from_hex)
                        {
                            Some(c) => c,
                            None => continue,
                        };
                        let tag = match rec
                            .get("tag")
                            .and_then(|x| x.as_str())
                            .and_then(crypto::from_hex)
                        {
                            Some(t) if t.len() == 16 => {
                                let mut a = [0u8; 16];
                                a.copy_from_slice(&t);
                                a
                            }
                            _ => continue,
                        };
                        let expires_at = rec
                            .get("expires_at")
                            .and_then(|x| x.as_i64())
                            .unwrap_or(0);
                        match crypto::gcm_decrypt(
                            &store.key,
                            &nonce,
                            &ct,
                            aad_for(user_id).as_bytes(),
                            &tag,
                        ) {
                            Some(plain) => {
                                if let Ok(text) = String::from_utf8(plain) {
                                    if let Some(pv) = parse_json(&text) {
                                        let access = pv
                                            .get("access_token")
                                            .and_then(|x| x.as_str())
                                            .unwrap_or("")
                                            .to_string();
                                        let refresh = pv
                                            .get("refresh_token")
                                            .and_then(|x| x.as_str())
                                            .unwrap_or("")
                                            .to_string();
                                        if !access.is_empty() && !refresh.is_empty() {
                                            store.tokens.insert(
                                                user_id,
                                                UserToken {
                                                    access_token: access,
                                                    refresh_token: refresh,
                                                    expires_at,
                                                },
                                            );
                                        }
                                    }
                                }
                            }
                            None => {
                                eprintln!(
                                    "[spotify-rust] WARNING: user {} token record failed to decrypt (key rotated?); skipping",
                                    user_id
                                );
                            }
                        }
                    }
                }
            }
        }
        if !store.tokens.is_empty() {
            eprintln!(
                "[spotify-rust] restored {} encrypted user token(s)",
                store.tokens.len()
            );
        }
        store
    }

    fn generate_and_store_key(key_path: &std::path::Path) -> [u8; 32] {
        let key = crypto::random_key_32();
        if let Some(parent) = key_path.parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        // 0600: the key file is the only thing standing between the
        // token file and plaintext.
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            if let Ok(mut f) = std::fs::OpenOptions::new()
                .write(true)
                .create(true)
                .truncate(true)
                .mode(0o600)
                .open(key_path)
            {
                use std::io::Write;
                let _ = f.write_all(crypto::to_hex(&key).as_bytes());
            }
        }
        #[cfg(not(unix))]
        {
            let _ = std::fs::write(key_path, crypto::to_hex(&key));
        }
        eprintln!(
            "[spotify-rust] generated fresh per-user token key at {}",
            key_path.display()
        );
        key
    }

    /// Persist all tokens (each freshly encrypted under a new random
    /// nonce). Atomic write; plaintext never touches disk.
    pub fn save(&self, data_dir: &std::path::Path) {
        let mut records = String::from("{\"users\":[");
        let mut first = true;
        for (user_id, tok) in &self.tokens {
            let plain = format!(
                "{{\"access_token\":{},\"refresh_token\":{}}}",
                quote(&tok.access_token),
                quote(&tok.refresh_token)
            );
            let nonce = crypto::random_nonce_12();
            let (ct, tag) = crypto::gcm_encrypt(
                &self.key,
                &nonce,
                plain.as_bytes(),
                aad_for(*user_id).as_bytes(),
            );
            if !first {
                records.push(',');
            }
            first = false;
            records.push_str(&format!(
                "{{\"user_id\":{},\"nonce\":{},\"ciphertext\":{},\"tag\":{},\"expires_at\":{}}}",
                user_id,
                quote(&crypto::to_hex(&nonce)),
                quote(&crypto::to_hex(&ct)),
                quote(&crypto::to_hex(&tag)),
                tok.expires_at
            ));
        }
        records.push_str("]}");
        let path = data_dir.join(TOKENS_FILE);
        if let Err(e) = atomic_write(&path, &records) {
            eprintln!(
                "[spotify-rust] failed to persist user tokens to {}: {}",
                path.display(),
                e
            );
        }
    }

    pub fn get(&self, user_id: i64) -> Option<&UserToken> {
        self.tokens.get(&user_id)
    }

    pub fn set(&mut self, user_id: i64, token: UserToken) {
        self.tokens.insert(user_id, token);
    }

    pub fn remove(&mut self, user_id: i64) -> bool {
        self.tokens.remove(&user_id).is_some()
    }

    pub fn len(&self) -> usize {
        self.tokens.len()
    }
}

// ------------------------------------------------------------ oauth ---

pub fn accounts_base() -> String {
    std::env::var("SPOTIFY_ACCOUNTS_URL")
        .ok()
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "https://accounts.spotify.com".to_string())
}

pub fn api_base() -> String {
    std::env::var("SPOTIFY_API_BASE")
        .ok()
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "https://api.spotify.com".to_string())
}

pub fn oauth_client() -> Option<(String, String, String)> {
    let id = std::env::var("SPOTIFY_CLIENT_ID").ok()?;
    let secret = std::env::var("SPOTIFY_CLIENT_SECRET").ok()?;
    let redirect = std::env::var("SPOTIFY_REDIRECT_URI")
        .ok()
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "http://localhost:8080/callback".to_string());
    if id.is_empty() || secret.is_empty() {
        return None;
    }
    Some((id, secret, redirect))
}

/// Spotify authorize URL for a user login. `state` must be an
/// unguessable per-user value (the caller passes the Telegram user id
/// plus a random token); Spotify returns it verbatim on the callback.
pub fn authorize_url(client_id: &str, redirect_uri: &str, state: &str) -> String {
    format!(
        "{}/authorize?client_id={}&response_type=code&redirect_uri={}&scope={}&state={}",
        accounts_base(),
        percent_encode(client_id),
        percent_encode(redirect_uri),
        percent_encode(OAUTH_SCOPES),
        percent_encode(state)
    )
}

/// POST a token-exchange/refresh form to the accounts endpoint.
/// Returns the parsed JSON on HTTP 200, None otherwise (the error
/// body is logged, never the secret).
fn post_token_form(base: &str, client_id: &str, secret: &str, form: &str) -> Option<Json> {
    let url = format!("{}/api/token", base.trim_end_matches('/'));
    let out = Command::new("curl")
        .arg("-sS")
        .arg("--max-time")
        .arg("15")
        .arg("-u")
        .arg(format!("{}:{}", client_id, secret))
        .arg("-d")
        .arg(form)
        .arg(&url)
        .output()
        .ok()?;
    if !out.status.success() {
        eprintln!(
            "[spotify-rust] token endpoint failed (HTTP {}): {}",
            out.status.code().unwrap_or(-1),
            String::from_utf8_lossy(&out.stdout).trim()
        );
        return None;
    }
    parse_json(String::from_utf8_lossy(&out.stdout).trim())
}

fn token_from_json(v: &Json, now: i64) -> Option<UserToken> {
    let access = v.get("access_token")?.as_str()?.to_string();
    let refresh = v.get("refresh_token").and_then(|x| x.as_str()).unwrap_or("").to_string();
    if access.is_empty() || refresh.is_empty() {
        return None;
    }
    let expires_in = v.get("expires_in").and_then(|x| x.as_i64()).unwrap_or(3600);
    Some(UserToken {
        access_token: access,
        refresh_token: refresh,
        expires_at: now + expires_in,
    })
}

/// Exchange an authorization code for tokens.
pub fn exchange_code(
    base: &str,
    client_id: &str,
    secret: &str,
    redirect_uri: &str,
    code: &str,
    now: i64,
) -> Option<UserToken> {
    let form = format!(
        "grant_type=authorization_code&code={}&redirect_uri={}",
        percent_encode(code),
        percent_encode(redirect_uri)
    );
    let v = post_token_form(base, client_id, secret, &form)?;
    token_from_json(&v, now)
}

/// Refresh an access token. Spotify may or may not return a new
/// refresh token; when it does not, the old one is kept (rustify does
/// the same — `Token.refresh_token` stays unless replaced).
pub fn refresh_token(
    base: &str,
    client_id: &str,
    secret: &str,
    old: &UserToken,
    now: i64,
) -> Option<UserToken> {
    let form = format!(
        "grant_type=refresh_token&refresh_token={}",
        percent_encode(&old.refresh_token)
    );
    let v = post_token_form(base, client_id, secret, &form)?;
    let access = v.get("access_token")?.as_str()?.to_string();
    if access.is_empty() {
        return None;
    }
    let refresh = v
        .get("refresh_token")
        .and_then(|x| x.as_str())
        .filter(|s| !s.is_empty())
        .unwrap_or(&old.refresh_token)
        .to_string();
    let expires_in = v.get("expires_in").and_then(|x| x.as_i64()).unwrap_or(3600);
    Some(UserToken {
        access_token: access,
        refresh_token: refresh,
        expires_at: now + expires_in,
    })
}

// ------------------------------------------------- currently playing ---

#[derive(Debug, Clone)]
pub struct NowPlayingTrack {
    pub id: String,
    pub title: String,
    pub artist: String,
    pub duration_sec: i64,
    pub is_playing: bool,
}

/// GET /v1/me/player/currently-playing with a user token.
/// Ok(None) = nothing playing (HTTP 204, or no item, or paused with
/// no item — rustify's `CurrentlyPlaying::None`). Err = the request
/// itself failed (network / 401 / unparsable); a 401 means the token
/// is dead and the caller should drop it.
pub fn currently_playing(
    base: &str,
    access_token: &str,
) -> Result<Option<NowPlayingTrack>, String> {
    let url = format!("{}/v1/me/player/currently-playing", base.trim_end_matches('/'));
    let out = Command::new("curl")
        .arg("-sS")
        .arg("--max-time")
        .arg("15")
        .arg("-H")
        .arg(format!("Authorization: Bearer {}", access_token))
        .arg("-w")
        .arg("\n%{http_code}")
        .arg(&url)
        .output()
        .map_err(|e| format!("curl failed: {}", e))?;
    let text = String::from_utf8_lossy(&out.stdout);
    let (body, status) = match text.rfind('\n') {
        Some(p) => (
            text[..p].to_string(),
            text[p + 1..].trim().parse::<u16>().unwrap_or(0),
        ),
        None => (text.to_string(), 0),
    };
    if status == 204 || body.trim().is_empty() {
        return Ok(None);
    }
    if status == 401 {
        return Err("unauthorized (token invalid or revoked)".to_string());
    }
    if status != 200 {
        return Err(format!("Spotify API HTTP {}", status));
    }
    let v = parse_json(body.trim())
        .ok_or_else(|| "unparsable currently-playing response".to_string())?;
    let is_playing = v.get("is_playing").and_then(|x| x.as_bool()).unwrap_or(false);
    let item = match v.get("item") {
        Some(it @ Json::Obj(_)) => it,
        _ => return Ok(None),
    };
    // Podcasts / local files have no usable track id — same as
    // rustify's CurrentlyPlaying::None(Podcast|Local).
    let id = match item.get("id").and_then(|x| x.as_str()) {
        Some(id) if !id.is_empty() => id.to_string(),
        _ => return Ok(None),
    };
    let title = item
        .get("name")
        .and_then(|x| x.as_str())
        .unwrap_or("")
        .to_string();
    let artists: Vec<String> = match item.get("artists") {
        Some(Json::Arr(items)) => items
            .iter()
            .filter_map(|a| a.get("name").and_then(|x| x.as_str()).map(|s| s.to_string()))
            .collect(),
        _ => Vec::new(),
    };
    let duration_sec = item
        .get("duration_ms")
        .and_then(|x| x.as_i64())
        .map(|ms| ms / 1000)
        .unwrap_or(0);
    Ok(Some(NowPlayingTrack {
        id,
        title,
        artist: artists.join(", "),
        duration_sec,
        is_playing,
    }))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};
    use std::net::TcpListener;
    use std::sync::mpsc;
    use std::thread;

    /// Tiny one-shot stub HTTP server: serves `responses` in order,
    /// each a (status, body) pair, on an ephemeral loopback port.
    /// Returns (base_url, thread handle).
    fn stub_server(responses: Vec<(u16, String)>) -> (String, thread::JoinHandle<()>) {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind stub");
        let port = listener.local_addr().unwrap().port();
        let (tx, rx) = mpsc::channel::<()>();
        let handle = thread::spawn(move || {
            let _ = tx.send(());
            for (status, body) in responses {
                let (mut stream, _) = match listener.accept() {
                    Ok(s) => s,
                    Err(_) => return,
                };
                let _ = stream.set_read_timeout(Some(std::time::Duration::from_secs(5)));
                let mut buf = [0u8; 8192];
                let mut req = Vec::new();
                loop {
                    match stream.read(&mut buf) {
                        Ok(0) => break,
                        Ok(n) => {
                            req.extend_from_slice(&buf[..n]);
                            if req.windows(4).any(|w| w == b"\r\n\r\n") {
                                break;
                            }
                        }
                        Err(_) => break,
                    }
                }
                // Read the body if a Content-Length was sent.
                let text = String::from_utf8_lossy(&req);
                if let Some(cl) = text
                    .lines()
                    .find(|l| l.to_ascii_lowercase().starts_with("content-length:"))
                {
                    if let Ok(len) = cl[15..].trim().parse::<usize>() {
                        let head_end = req
                            .windows(4)
                            .position(|w| w == b"\r\n\r\n")
                            .map(|p| p + 4)
                            .unwrap_or(req.len());
                        let mut have = req.len() - head_end;
                        while have < len {
                            match stream.read(&mut buf) {
                                Ok(0) => break,
                                Ok(n) => have += n,
                                Err(_) => break,
                            }
                        }
                    }
                }
                let reason = match status {
                    200 => "OK",
                    204 => "No Content",
                    400 => "Bad Request",
                    401 => "Unauthorized",
                    _ => "OK",
                };
                let resp = format!(
                    "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                    status,
                    reason,
                    body.len(),
                    body
                );
                let _ = stream.write_all(resp.as_bytes());
                let _ = stream.flush();
            }
        });
        let _ = rx.recv();
        (format!("http://127.0.0.1:{}", port), handle)
    }

    fn tmp_data_dir(tag: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir()
            .join(format!("parlay-spotify-auth-test-{}-{}", std::process::id(), tag));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).expect("tmp dir");
        dir
    }

    #[test]
    fn store_encrypt_roundtrip() {
        let dir = tmp_data_dir("roundtrip");
        let mut store = TokenStore::load(&dir);
        assert_eq!(store.len(), 0);
        store.set(
            12345,
            UserToken {
                access_token: "access-abc".to_string(),
                refresh_token: "refresh-xyz".to_string(),
                expires_at: 1_800_000_000,
            },
        );
        store.save(&dir);
        // The token file must not contain the plaintext tokens.
        let raw = std::fs::read_to_string(dir.join(TOKENS_FILE)).unwrap();
        assert!(!raw.contains("access-abc"), "access token leaked to disk");
        assert!(!raw.contains("refresh-xyz"), "refresh token leaked to disk");
        assert!(raw.contains("12345"));
        // A fresh load with the same key file decrypts them back.
        let store2 = TokenStore::load(&dir);
        let tok = store2.get(12345).expect("token restored");
        assert_eq!(tok.access_token, "access-abc");
        assert_eq!(tok.refresh_token, "refresh-xyz");
        assert_eq!(tok.expires_at, 1_800_000_000);
        // The key file is 0600.
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = std::fs::metadata(dir.join(KEY_FILE)).unwrap().permissions().mode();
            assert_eq!(mode & 0o777, 0o600, "key file must be 0600");
        }
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn store_wrong_key_skips_record() {
        let dir = tmp_data_dir("wrongkey");
        let mut store = TokenStore::load(&dir);
        store.set(
            7,
            UserToken {
                access_token: "tok".to_string(),
                refresh_token: "ref".to_string(),
                expires_at: 999,
            },
        );
        store.save(&dir);
        // Rotate the key file: records must be skipped, not crash.
        std::fs::remove_file(dir.join(KEY_FILE)).unwrap();
        let store2 = TokenStore::load(&dir);
        assert_eq!(store2.len(), 0, "undecryptable record must be skipped");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn token_refresh_window() {
        let tok = UserToken {
            access_token: "a".into(),
            refresh_token: "r".into(),
            expires_at: 1000,
        };
        assert!(tok.needs_refresh(900)); // within 120 s
        assert!(!tok.needs_refresh(800));
    }

    #[test]
    fn authorize_url_shape() {
        let url = authorize_url("cid", "http://localhost:8080/callback", "state-1");
        assert!(url.starts_with("https://accounts.spotify.com/authorize?"));
        assert!(url.contains("response_type=code"));
        assert!(url.contains("scope=user-read-currently-playing%20user-read-playback-state"));
        assert!(url.contains("state=state-1"));
    }

    #[test]
    fn exchange_code_with_stubbed_endpoint() {
        let (base, handle) = stub_server(vec![(
            200,
            r#"{"access_token":"stub-access","refresh_token":"stub-refresh","expires_in":3600,"token_type":"Bearer"}"#.to_string(),
        )]);
        let tok = exchange_code(&base, "cid", "secret", "http://localhost/cb", "code-1", 1_000_000)
            .expect("exchange succeeds against stub");
        assert_eq!(tok.access_token, "stub-access");
        assert_eq!(tok.refresh_token, "stub-refresh");
        assert_eq!(tok.expires_at, 1_000_000 + 3600);
        handle.join().unwrap();
    }

    #[test]
    fn refresh_with_stubbed_endpoint_keeps_old_refresh_token() {
        // Spotify often omits refresh_token on refresh; the old one must
        // be kept (rustify behaviour).
        let (base, handle) = stub_server(vec![(
            200,
            r#"{"access_token":"new-access","expires_in":3600,"token_type":"Bearer"}"#.to_string(),
        )]);
        let old = UserToken {
            access_token: "old".into(),
            refresh_token: "keep-me".into(),
            expires_at: 500,
        };
        let tok = refresh_token(&base, "cid", "secret", &old, 1_000_000)
            .expect("refresh succeeds against stub");
        assert_eq!(tok.access_token, "new-access");
        assert_eq!(tok.refresh_token, "keep-me");
        assert_eq!(tok.expires_at, 1_000_000 + 3600);
        handle.join().unwrap();
    }

    #[test]
    fn refresh_failure_is_none() {
        let (base, handle) = stub_server(vec![(
            400,
            r#"{"error":"invalid_grant","error_description":"Refresh token revoked"}"#.to_string(),
        )]);
        let old = UserToken {
            access_token: "old".into(),
            refresh_token: "dead".into(),
            expires_at: 500,
        };
        assert!(refresh_token(&base, "cid", "secret", &old, 1_000_000).is_none());
        handle.join().unwrap();
    }

    #[test]
    fn currently_playing_parses_stub() {
        let (base, handle) = stub_server(vec![(
            200,
            r#"{"is_playing":true,"item":{"id":"track1","name":"Song","duration_ms":213000,"artists":[{"name":"Rick Astley"}]}}"#.to_string(),
        )]);
        let np = currently_playing(&base, "tok").expect("request ok");
        let np = np.expect("track playing");
        assert_eq!(np.id, "track1");
        assert_eq!(np.title, "Song");
        assert_eq!(np.artist, "Rick Astley");
        assert_eq!(np.duration_sec, 213);
        assert!(np.is_playing);
        handle.join().unwrap();
    }

    #[test]
    fn currently_playing_empty_means_nothing() {
        let (base, handle) = stub_server(vec![(204, String::new())]);
        let np = currently_playing(&base, "tok").expect("request ok");
        assert!(np.is_none(), "204 = nothing playing");
        handle.join().unwrap();
    }

    #[test]
    fn currently_playing_401_is_error() {
        let (base, handle) = stub_server(vec![(
            401,
            r#"{"error":{"status":401,"message":"The access token expired"}}"#.to_string(),
        )]);
        assert!(currently_playing(&base, "dead-token").is_err());
        handle.join().unwrap();
    }
}
