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

fn handle_vote(state: &Arc<Mutex<Store>>, req: &Request) -> (u16, String) {
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
        move |req: &Request| -> (u16, String) { handle_vote(&state, req) }
    };

    if let Err(e) = run_server(port, SERVICE, handler) {
        eprintln!("parlay {} failed to listen on {}: {}", SERVICE, port, e);
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn req(body: &str) -> Request {
        Request {
            method: "POST".to_string(),
            path: "/v1/queue/vote".to_string(),
            headers: Vec::new(),
            body: body.to_string(),
        }
    }

    /// Fresh store (threshold 3 unless given) backed by a unique temp file.
    fn tmp_store(name: &str, threshold: usize) -> (Arc<Mutex<Store>>, std::path::PathBuf) {
        let file = std::env::temp_dir()
            .join(format!("parlay-queue-test-{}-{}.json", std::process::id(), name));
        let _ = std::fs::remove_file(&file);
        (Arc::new(Mutex::new(Store::load(file.clone(), threshold))), file)
    }

    fn vote(state: &Arc<Mutex<Store>>, track_id: &str, vote: &str, voter: &str) -> (u16, Json) {
        let body = format!(
            r#"{{"track":{{"id":{},"title":"T"}},"vote":{},"voter_id":{}}}"#,
            quote(track_id),
            quote(vote),
            quote(voter)
        );
        let (st, resp) = handle_vote(state, &req(&body));
        let v = parse_json(&resp).expect("response parses");
        (st, v)
    }

    fn data(v: &Json) -> &Json {
        assert_eq!(v.get("ok").and_then(|x| x.as_bool()), Some(true), "ok:true in {}", v.render());
        v.get("data").expect("data present")
    }

    #[test]
    fn threshold_flip_at_3() {
        let (state, file) = tmp_store("flip", 3);
        let (st, v) = vote(&state, "trk-1", "approve", "u1");
        assert_eq!(st, 200);
        assert_eq!(data(&v).get("queued").and_then(|x| x.as_bool()), Some(false));
        assert_eq!(data(&v).get("approvals").and_then(|x| x.as_i64()), Some(1));
        assert_eq!(data(&v).get("threshold").and_then(|x| x.as_i64()), Some(3));
        let (_, v) = vote(&state, "trk-1", "approve", "u2");
        assert_eq!(data(&v).get("queued").and_then(|x| x.as_bool()), Some(false));
        assert_eq!(data(&v).get("approvals").and_then(|x| x.as_i64()), Some(2));
        // Third distinct approval flips queued.
        let (_, v) = vote(&state, "trk-1", "approve", "u3");
        assert_eq!(data(&v).get("queued").and_then(|x| x.as_bool()), Some(true));
        assert_eq!(data(&v).get("approvals").and_then(|x| x.as_i64()), Some(3));
        assert_eq!(data(&v).get("track_id").and_then(|x| x.as_str()), Some("trk-1"));
        let _ = std::fs::remove_file(&file);
    }

    #[test]
    fn custom_threshold_1_queues_on_first_vote() {
        let (state, file) = tmp_store("threshold1", 1);
        let (_, v) = vote(&state, "trk-9", "approve", "u1");
        assert_eq!(data(&v).get("queued").and_then(|x| x.as_bool()), Some(true));
        assert_eq!(data(&v).get("threshold").and_then(|x| x.as_i64()), Some(1));
        let _ = std::fs::remove_file(&file);
    }

    #[test]
    fn re_vote_does_not_double_count() {
        let (state, file) = tmp_store("revote", 3);
        vote(&state, "trk-2", "approve", "u1");
        let (_, v) = vote(&state, "trk-2", "approve", "u1");
        assert_eq!(data(&v).get("approvals").and_then(|x| x.as_i64()), Some(1));
        assert_eq!(data(&v).get("queued").and_then(|x| x.as_bool()), Some(false));
        let _ = std::fs::remove_file(&file);
    }

    #[test]
    fn decline_cancels_approval() {
        let (state, file) = tmp_store("decline", 3);
        vote(&state, "trk-3", "approve", "u1");
        vote(&state, "trk-3", "approve", "u2");
        let (_, v) = vote(&state, "trk-3", "decline", "u1");
        assert_eq!(data(&v).get("approvals").and_then(|x| x.as_i64()), Some(1));
        assert_eq!(data(&v).get("declines").and_then(|x| x.as_i64()), Some(1));
        assert_eq!(data(&v).get("queued").and_then(|x| x.as_bool()), Some(false));
        // Re-approving after a decline restores the approval.
        let (_, v) = vote(&state, "trk-3", "approve", "u1");
        assert_eq!(data(&v).get("approvals").and_then(|x| x.as_i64()), Some(2));
        assert_eq!(data(&v).get("declines").and_then(|x| x.as_i64()), Some(0));
        let _ = std::fs::remove_file(&file);
    }

    #[test]
    fn decline_only_never_queues() {
        let (state, file) = tmp_store("declineonly", 3);
        for voter in ["u1", "u2", "u3"] {
            let (st, v) = vote(&state, "trk-4", "decline", voter);
            assert_eq!(st, 200);
            assert_eq!(data(&v).get("queued").and_then(|x| x.as_bool()), Some(false));
        }
        let (_, v) = vote(&state, "trk-4", "decline", "u4");
        assert_eq!(data(&v).get("approvals").and_then(|x| x.as_i64()), Some(0));
        assert_eq!(data(&v).get("declines").and_then(|x| x.as_i64()), Some(4));
        let _ = std::fs::remove_file(&file);
    }

    #[test]
    fn votes_are_per_track() {
        let (state, file) = tmp_store("pertrack", 3);
        vote(&state, "trk-a", "approve", "u1");
        vote(&state, "trk-a", "approve", "u2");
        let (_, v) = vote(&state, "trk-b", "approve", "u1");
        assert_eq!(data(&v).get("approvals").and_then(|x| x.as_i64()), Some(1));
        let _ = std::fs::remove_file(&file);
    }

    #[test]
    fn handle_rejects_bad_requests() {
        let (state, file) = tmp_store("badreq", 3);
        let (st, _) = handle_vote(&state, &req(r#"{"track":{"id":"x"},"vote":"maybe","voter_id":"u1"}"#));
        assert_eq!(st, 400, "vote must be approve|decline");
        let (st, _) = handle_vote(&state, &req(r#"{"track":{"id":"x"},"vote":"approve"}"#));
        assert_eq!(st, 400, "voter_id required");
        let (st, _) = handle_vote(&state, &req(r#"{"vote":"approve","voter_id":"u1"}"#));
        assert_eq!(st, 400, "track required");
        let (st, _) = handle_vote(&state, &req("not json"));
        assert_eq!(st, 400);
        let bad_route = Request {
            method: "GET".to_string(),
            path: "/v1/queue/vote".to_string(),
            headers: Vec::new(),
            body: String::new(),
        };
        let (st, _) = handle_vote(&state, &bad_route);
        assert_eq!(st, 404);
        let _ = std::fs::remove_file(&file);
    }

    #[test]
    fn votes_persist_across_reload() {
        let (state, file) = tmp_store("persist", 3);
        vote(&state, "trk-5", "approve", "u1");
        vote(&state, "trk-5", "approve", "u2");
        drop(state);
        let reloaded = Arc::new(Mutex::new(Store::load(file.clone(), 3)));
        let (_, v) = vote(&reloaded, "trk-5", "approve", "u3");
        assert_eq!(data(&v).get("approvals").and_then(|x| x.as_i64()), Some(3));
        assert_eq!(data(&v).get("queued").and_then(|x| x.as_bool()), Some(true));
        let _ = std::fs::remove_file(&file);
    }
}
