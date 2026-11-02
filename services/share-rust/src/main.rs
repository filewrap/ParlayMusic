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

use common::*;
use std::collections::HashMap;
use std::sync::{Arc, Mutex};

const SERVICE: &str = "share-rust";

struct Store {
    map: HashMap<String, Json>, // token -> track object
    file: std::path::PathBuf,
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
        Store { map, file }
    }

    fn save(&self) -> std::io::Result<()> {
        let mut pairs: Vec<(String, Json)> =
            self.map.iter().map(|(k, v)| (k.clone(), v.clone())).collect();
        pairs.sort_by(|a, b| a.0.cmp(&b.0));
        atomic_write(&self.file, &Json::Obj(pairs).render())
    }
}

fn main() {
    let port: u16 = std::env::var("PORT")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(8004);
    let bot = std::env::var("PARLAY_BOT_USERNAME").unwrap_or_else(|_| "ParlayBot".to_string());
    let state = Arc::new(Mutex::new(Store::load(data_file("shares.json"))));
    let bot = Arc::new(bot);

    let handler = {
        let state = Arc::clone(&state);
        let bot = Arc::clone(&bot);
        move |req: &Request| -> (u16, String) {
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
    };

    if let Err(e) = run_server(port, SERVICE, handler) {
        eprintln!("parlay {} failed to listen on {}: {}", SERVICE, port, e);
        std::process::exit(1);
    }
}
