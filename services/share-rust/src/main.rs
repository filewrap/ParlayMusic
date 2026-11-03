//! parlay-share — anonymous music sharing.
//!
//! Ported (behaviour only, re-implemented) from refs/feel-me
//! (`src/main.rs`, `src/models.rs`): feel-me mints a unique link for a
//! track so it can be shared without attaching an identity to the link.
//! The reference uses teloxide + Diesel/Postgres; Parlay keeps the same
//! behaviour behind the IPC contract (contracts/ipc.json `share.create` /
//! `share.resolve`) with an in-memory map + a JSON file instead:
//!
//!   POST /v1/share        {created_by, track} -> {link, token}
//!   GET  /v1/share/:token                     -> {track}
//!
//! Tokens are 16 random alphanumeric chars from /dev/urandom.
//! Link: https://t.me/<bot>?start=share_<token>, bot username from
//! $PARLAY_BOT_USERNAME (default "ParlayBot").
//!
//! Anonymity: `created_by` is accepted (contract field) but deliberately
//! NOT stored — the stored record is token -> track only, so a link
//! carries no identity. No Telegram call is made here; this service only
//! mints the token/link. Persistence is ./data/shares.json
//! ($PARLAY_DATA_DIR override), written atomically (tmp + rename).

mod common;
mod store;

use common::*;
use std::collections::HashMap;
use std::sync::{Arc, Mutex};

const SERVICE: &str = "share-rust";

struct Store {
    map: HashMap<String, Json>, // token -> track object
    file: std::path::PathBuf,
    /// True when this store persists to the shared store (A11) instead
    /// of the file. Decided once at construction; tests that build a
    /// file store are unaffected by any global backend the live tests
    /// initialise.
    shared: bool,
}

impl Store {
    fn load(file: std::path::PathBuf) -> Self {
        let mut map = HashMap::new();
        if let Ok(text) = std::fs::read_to_string(&file) {
            if let Some(Json::Obj(pairs)) = parse_json(&text) {
                for (k, v) in pairs {
                    map.insert(k, v);
                }
            }
        }
        Store {
            map,
            file,
            shared: false,
        }
    }

    /// Load from the shared store (A11), key `share:links`. Same JSON
    /// shape as shares.json (token -> track object).
    fn load_shared() -> Self {
        let mut map = HashMap::new();
        match store::get("share:links") {
            Ok(Some(text)) => {
                if let Some(Json::Obj(pairs)) = parse_json(&text) {
                    for (k, v) in pairs {
                        map.insert(k, v);
                    }
                }
            }
            Ok(None) => {}
            Err(e) => eprintln!("[share-rust] shared store load failed: {}", e),
        }
        Store {
            map,
            file: std::path::PathBuf::new(),
            shared: true,
        }
    }

    fn render(&self) -> String {
        let mut pairs: Vec<(String, Json)> =
            self.map.iter().map(|(k, v)| (k.clone(), v.clone())).collect();
        pairs.sort_by(|a, b| a.0.cmp(&b.0));
        Json::Obj(pairs).render()
    }

    fn save(&self) -> std::io::Result<()> {
        // Shared store (A11): the whole token map is one value under
        // key `share:links`. The JSON-file path below is unchanged.
        if self.shared {
            return store::set("share:links", &self.render())
                .map_err(|e| std::io::Error::new(std::io::ErrorKind::Other, e));
        }
        atomic_write(&self.file, &self.render())
    }
}

fn handle(state: &Arc<Mutex<Store>>, bot: &str, req: &Request) -> (u16, String) {
    if req.method == "POST" && req.path == "/v1/share" {
        let body = match parse_json(&req.body) {
            Some(b @ Json::Obj(_)) => b,
            _ => return (400, err("bad_request", "invalid JSON body")),
        };
        // created_by is accepted per contract but not stored:
        // links are anonymous by design (see module docs).
        let track = match body.get("track") {
            Some(t @ Json::Obj(_)) => t.clone(),
            _ => {
                return (
                    400,
                    err("bad_request", "missing object field: track"),
                )
            }
        };
        let mut store = state.lock().unwrap();
        let mut token = random_token(16);
        while store.map.contains_key(&token) {
            token = random_token(16);
        }
        store.map.insert(token.clone(), track);
        if let Err(e) = store.save() {
            store.map.remove(&token);
            return (
                500,
                err("internal", &format!("persist failed: {}", e)),
            );
        }
        let link = format!("https://t.me/{}?start=share_{}", bot, token);
        (
            200,
            ok(&format!(
                "{{\"link\":{},\"token\":{}}}",
                quote(&link),
                quote(&token)
            )),
        )
    } else if req.method == "GET" {
        if let Some(tok) = req.path.strip_prefix("/v1/share/") {
            if tok.is_empty() || tok.contains('/') {
                return (404, err("not_found", "unknown share token"));
            }
            let store = state.lock().unwrap();
            return match store.map.get(tok) {
                Some(track) => {
                    (200, ok(&format!("{{\"track\":{}}}", track.render())))
                }
                None => (404, err("not_found", "unknown share token")),
            };
        }
        (404, err("not_found", "route not found (GET /v1/share/:token)"))
    } else {
        (404, err("not_found", "route not found"))
    }
}

fn main() {
    let port: u16 = std::env::var("PORT")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(8004);
    let bot = std::env::var("PARLAY_BOT_USERNAME").unwrap_or_else(|_| "ParlayBot".to_string());
    // Shared durable store (A11): Mongo > Redis > JSON file. A
    // configured-but-unreachable backend is fatal — see store.rs.
    if let Err(e) = store::init() {
        eprintln!("[share-rust] shared store init failed: {}", e);
        std::process::exit(1);
    }
    let state = if store::backend() != "file" {
        Arc::new(Mutex::new(Store::load_shared()))
    } else {
        Arc::new(Mutex::new(Store::load(data_file("shares.json"))))
    };
    let bot = Arc::new(bot);

    let handler = {
        let state = Arc::clone(&state);
        let bot = Arc::clone(&bot);
        move |req: &Request| -> (u16, String) { handle(&state, &bot, req) }
    };

    if let Err(e) = run_server(port, SERVICE, handler) {
        eprintln!("parlay {} failed to listen on {}: {}", SERVICE, port, e);
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn req(method: &str, path: &str, body: &str) -> Request {
        Request {
            method: method.to_string(),
            path: path.to_string(),
            headers: Vec::new(),
            body: body.to_string(),
        }
    }

    /// Fresh store backed by a unique temp file (tests run in parallel).
    fn tmp_store(name: &str) -> (Arc<Mutex<Store>>, std::path::PathBuf) {
        let file = std::env::temp_dir()
            .join(format!("parlay-share-test-{}-{}.json", std::process::id(), name));
        let _ = std::fs::remove_file(&file);
        (Arc::new(Mutex::new(Store::load(file.clone()))), file)
    }

    fn data_field(body: &str, key: &str) -> Json {
        let v = parse_json(body).expect("response parses");
        assert_eq!(v.get("ok").and_then(|x| x.as_bool()), Some(true), "ok:true in {body}");
        v.get("data").and_then(|d| d.get(key)).cloned().expect("data field present")
    }

    const TRACK: &str = r#"{"id":"trk-1","title":"Test Song","artist":"Test Artist"}"#;

    #[test]
    fn create_resolve_round_trip() {
        let (state, file) = tmp_store("roundtrip");
        let body = format!(r#"{{"created_by":987654321,"track":{}}}"#, TRACK);
        let (st, resp) = handle(&state, "ParlayBot", &req("POST", "/v1/share", &body));
        assert_eq!(st, 200, "{resp}");
        let token = data_field(&resp, "token").as_str().unwrap().to_string();
        let link = data_field(&resp, "link").as_str().unwrap().to_string();
        assert_eq!(token.len(), 16, "token is 16 chars");
        assert!(token.chars().all(|c| c.is_ascii_alphanumeric()), "token is alnum");
        assert_eq!(link, format!("https://t.me/ParlayBot?start=share_{}", token));

        // Resolve returns the same track, byte-for-byte in content.
        let (st, resp) = handle(&state, "ParlayBot", &req("GET", &format!("/v1/share/{}", token), ""));
        assert_eq!(st, 200, "{resp}");
        let track = data_field(&resp, "track");
        assert_eq!(track.get("id").and_then(|x| x.as_str()), Some("trk-1"));
        assert_eq!(track.get("title").and_then(|x| x.as_str()), Some("Test Song"));
        assert_eq!(track.get("artist").and_then(|x| x.as_str()), Some("Test Artist"));

        // Anonymity: created_by was accepted but is NOT in the persisted file.
        let persisted = std::fs::read_to_string(&file).expect("shares file written");
        assert!(!persisted.contains("987654321"), "created_by must not be stored");
        assert!(persisted.contains("trk-1"), "track is stored");
        let _ = std::fs::remove_file(&file);
    }

    #[test]
    fn two_creates_give_different_tokens() {
        let (state, file) = tmp_store("distinct");
        let body = format!(r#"{{"track":{}}}"#, TRACK);
        let (_, r1) = handle(&state, "ParlayBot", &req("POST", "/v1/share", &body));
        let (_, r2) = handle(&state, "ParlayBot", &req("POST", "/v1/share", &body));
        let t1 = data_field(&r1, "token").as_str().unwrap().to_string();
        let t2 = data_field(&r2, "token").as_str().unwrap().to_string();
        assert_ne!(t1, t2, "tokens are unique");
        let _ = std::fs::remove_file(&file);
    }

    #[test]
    fn unknown_token_404() {
        let (state, file) = tmp_store("unknown");
        let (st, _) = handle(&state, "ParlayBot", &req("GET", "/v1/share/doesnotexist1234", ""));
        assert_eq!(st, 404);
        let (st, _) = handle(&state, "ParlayBot", &req("GET", "/v1/share/", ""));
        assert_eq!(st, 404);
        let (st, _) = handle(&state, "ParlayBot", &req("GET", "/v1/share/a/b", ""));
        assert_eq!(st, 404);
        let _ = std::fs::remove_file(&file);
    }

    #[test]
    fn create_requires_track_object() {
        let (state, file) = tmp_store("badtrack");
        let (st, _) = handle(&state, "ParlayBot", &req("POST", "/v1/share", r#"{"track":"nope"}"#));
        assert_eq!(st, 400);
        let (st, _) = handle(&state, "ParlayBot", &req("POST", "/v1/share", r#"{}"#));
        assert_eq!(st, 400);
        let (st, _) = handle(&state, "ParlayBot", &req("POST", "/v1/share", "not json"));
        assert_eq!(st, 400);
        let (st, _) = handle(&state, "ParlayBot", &req("DELETE", "/v1/share", "{}"));
        assert_eq!(st, 404);
        let _ = std::fs::remove_file(&file);
    }

    #[test]
    fn store_survives_reload() {
        let (state, file) = tmp_store("reload");
        let body = format!(r#"{{"track":{}}}"#, TRACK);
        let (_, resp) = handle(&state, "ParlayBot", &req("POST", "/v1/share", &body));
        let token = data_field(&resp, "token").as_str().unwrap().to_string();
        drop(state);
        // A fresh Store over the same file still resolves the token.
        let reloaded = Arc::new(Mutex::new(Store::load(file.clone())));
        let (st, resp) = handle(&reloaded, "ParlayBot", &req("GET", &format!("/v1/share/{}", token), ""));
        assert_eq!(st, 200, "{resp}");
        let _ = std::fs::remove_file(&file);
    }

    /// A11: a link minted into the shared store still resolves after a
    /// reload from a fresh Store. Uses whichever of
    /// PARLAY_TEST_MONGO_URI / PARLAY_TEST_REDIS_URL is set; skipped
    /// when neither is (Mongo wins when both are set).
    #[test]
    fn link_survives_shared_store_reload() {
        let mongo = std::env::var("PARLAY_TEST_MONGO_URI").unwrap_or_default();
        let redis = std::env::var("PARLAY_TEST_REDIS_URL").unwrap_or_default();
        if mongo.is_empty() && redis.is_empty() {
            eprintln!("SKIP link_survives_shared_store_reload (no PARLAY_TEST_*_URI)");
            return;
        }
        store::init_from(&mongo, &redis).expect("shared store init");
        assert_ne!(store::backend(), "file", "backend must be shared");
        store::del("share:links").expect("clean slate");
        let state = Arc::new(Mutex::new(Store::load_shared()));
        let body = format!(r#"{{"track":{}}}"#, TRACK);
        let (_, resp) = handle(&state, "ParlayBot", &req("POST", "/v1/share", &body));
        let token = data_field(&resp, "token").as_str().unwrap().to_string();
        drop(state);
        let reloaded = Arc::new(Mutex::new(Store::load_shared()));
        let (st, resp) = handle(&reloaded, "ParlayBot", &req("GET", &format!("/v1/share/{}", token), ""));
        assert_eq!(st, 200, "{resp}");
        let track = data_field(&resp, "track");
        assert_eq!(track.get("id").and_then(|x| x.as_str()), Some("trk-1"));
        store::del("share:links").expect("cleanup");
    }
}
