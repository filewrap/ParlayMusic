//! parlay-queue — crowd-curated voting queue.
//!
//! Ported (behaviour only, re-implemented) from
//! refs/telegram-music-bot-daslui `src/spotify.rs`: daslui posts a Spotify
//! track link into a voting chat; members vote approve/decline and an
//! approved track joins the Spotify playback queue. The reference needs
//! live Spotify OAuth (rspotify) and a Telegram voting chat; Parlay keeps
//! the voting behaviour behind the IPC contract (contracts/ipc.json
//! `queue.vote`) with no Spotify/Telegram calls — the core owns playback,
//! this service only decides when a track is crowd-approved:
//!
//!   POST /v1/queue/vote {track, vote: approve|decline, voter_id}
//!     -> {queued, approvals, declines, threshold, track_id}
//!
//! Rules: one vote per voter per track (a re-vote replaces the previous
//! one); a decline cancels that voter's approval; a track is queued once
//! approvals >= threshold ($PARLAY_VOTE_THRESHOLD, default 3).
//! State is in-memory + ./data/votes.json ($PARLAY_DATA_DIR override),
//! written atomically (tmp + rename). Tracks are keyed by track.id
//! (falling back to the rendered track JSON when id is absent).

mod common;

use common::*;
use std::collections::HashMap;
use std::sync::{Arc, Mutex};

const SERVICE: &str = "queue-rust";

struct Entry {
    track: Json,
    approvers: Vec<String>,
    decliners: Vec<String>,
}

struct Store {
    map: HashMap<String, Entry>, // track key -> votes
    file: std::path::PathBuf,
    threshold: usize,
}

impl Store {
    fn load(file: std::path::PathBuf, threshold: usize) -> Self {
        let mut map: HashMap<String, Entry> = HashMap::new();
        if let Ok(text) = std::fs::read_to_string(&file) {
            if let Some(Json::Obj(pairs)) = parse_json(&text) {
                for (key, v) in pairs {
                    let track = v.get("track").cloned().unwrap_or(Json::Null);
                    let str_list = |name: &str| -> Vec<String> {
                        match v.get(name) {
                            Some(Json::Arr(items)) => items
                                .iter()
                                .filter_map(|i| i.as_str().map(|s| s.to_string()))
                                .collect(),
                            _ => Vec::new(),
                        }
                    };
                    map.insert(
                        key,
                        Entry {
                            track,
                            approvers: str_list("approvals"),
                            decliners: str_list("declines"),
                        },
                    );
                }
            }
        }
        Store {
            map,
            file,
            threshold,
        }
    }

    fn save(&self) -> std::io::Result<()> {
        let mut keys: Vec<&String> = self.map.keys().collect();
        keys.sort();
        let pairs: Vec<(String, Json)> = keys
            .iter()
            .map(|k| {
                let e = &self.map[*k];
                let val = Json::Obj(vec![
                    ("track".to_string(), e.track.clone()),
                    (
                        "approvals".to_string(),
                        Json::Arr(
                            e.approvers
                                .iter()
                                .map(|s| Json::Str(s.clone()))
                                .collect(),
                        ),
                    ),
                    (
                        "declines".to_string(),
                        Json::Arr(
                            e.decliners
                                .iter()
                                .map(|s| Json::Str(s.clone()))
                                .collect(),
                        ),
                    ),
                ]);
                ((*k).clone(), val)
            })
            .collect();
        atomic_write(&self.file, &Json::Obj(pairs).render())
    }
}

fn voter_id_of(v: &Json) -> Option<String> {
    match v {
        Json::Str(s) => Some(s.clone()),
        Json::Num(_) => v.as_i64().map(|n| n.to_string()),
        _ => None,
    }
}

fn main() {
    let port: u16 = std::env::var("PORT")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(8005);
    let threshold: usize = std::env::var("PARLAY_VOTE_THRESHOLD")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(3);
    let state = Arc::new(Mutex::new(Store::load(
        data_file("votes.json"),
        threshold,
    )));

    let handler = {
        let state = Arc::clone(&state);
        move |req: &Request| -> (u16, String) {
            if req.method != "POST" || req.path != "/v1/queue/vote" {
                return (404, err("not_found", "route not found (POST /v1/queue/vote)"));
            }
            let body = match parse_json(&req.body) {
                Some(b @ Json::Obj(_)) => b,
                _ => return (400, err("bad_request", "invalid JSON body")),
            };
            let track = match body.get("track") {
                Some(t @ Json::Obj(_)) => t.clone(),
                _ => {
                    return (
                        400,
                        err("bad_request", "missing object field: track"),
                    )
                }
            };
            let vote = match body.get("vote").and_then(|v| v.as_str()) {
                Some("approve") => "approve",
                Some("decline") => "decline",
                _ => {
                    return (
                        400,
                        err("bad_request", "vote must be approve|decline"),
                    )
                }
            };
            let voter = match body.get("voter_id").and_then(voter_id_of) {
                Some(v) => v,
                None => {
                    return (
                        400,
                        err("bad_request", "missing field: voter_id"),
                    )
                }
            };
            let key = track
                .get("id")
                .and_then(|v| v.as_str())
                .map(|s| s.to_string())
                .unwrap_or_else(|| track.render());

            let mut store = state.lock().unwrap();
            let entry = store.map.entry(key.clone()).or_insert(Entry {
                track: track.clone(),
                approvers: Vec::new(),
                decliners: Vec::new(),
            });
            // One vote per voter per track: a new vote replaces the old one.
            match vote {
                "approve" => {
                    if !entry.approvers.contains(&voter) {
                        entry.approvers.push(voter.clone());
                    }
                    entry.decliners.retain(|d| d != &voter);
                }
                _ => {
                    if !entry.decliners.contains(&voter) {
                        entry.decliners.push(voter.clone());
                    }
                    // A decline cancels that voter's approval.
                    entry.approvers.retain(|a| a != &voter);
                }
            }
            let approvals = entry.approvers.len();
            let declines = entry.decliners.len();
            let queued = approvals >= store.threshold;
            if let Err(e) = store.save() {
                return (
                    500,
                    err("internal", &format!("persist failed: {}", e)),
                );
            }
            (
                200,
                ok(&format!(
                    "{{\"queued\":{},\"approvals\":{},\"declines\":{},\"threshold\":{},\"track_id\":{}}}",
                    queued,
                    approvals,
                    declines,
                    store.threshold,
                    quote(&key)
                )),
            )
        }
    };

    if let Err(e) = run_server(port, SERVICE, handler) {
        eprintln!("parlay {} failed to listen on {}: {}", SERVICE, port, e);
        std::process::exit(1);
    }
}
