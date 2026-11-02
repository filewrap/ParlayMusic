//! Gemini AI classifier (Phase 5).
//!
//! Rustify's AI-slop detection (`src/services/ai_slop_detection`) asks
//! external detectors (SoulOverAI / Spotify AI Blocker / SHLabs) whether
//! a track's lyrics look AI-written, and its profanity check is the
//! rustrict trie. Parlay has neither detector wired, so `skip-check`
//! could only ever say `profanity` (wordlist) or `disliked` —
//! `reason:"ai-generated"` was never emitted.
//!
//! This module adds one classifier: Google's Gemini API, env-gated on
//! `GEMINI_API_KEY` (name only — the value is never logged, never
//! stored, passed to curl as an `x-goog-api-key` header). It classifies
//! the track text (title + artist + lyrics when available) into
//! clean / profanity / ai_generated, with a confidence. The prompt
//! asks for JSON only (`responseMimeType: application/json` + a
//! response schema), so the verdict is machine-readable.
//!
//! Verdict rules — NEVER fabricate:
//!   * Key set + Gemini answers: the Gemini verdict wins (it replaces
//!     the wordlist-only verdict, per the Phase 5 brief).
//!   * Key set + Gemini errors/unparsable: fall back to the wordlist,
//!     provider reported as `wordlist-fallback`.
//!   * No key: wordlist only, provider `wordlist`.
//!   * No key AND no lyrics: the AI verdict is `unknown` — the
//!     wordlist can still judge profanity on title/artist, but nothing
//!     here claims to know whether lyrics are AI-written without
//!     either lyrics or a classifier. `skip-check` reports
//!     `classifier_verdict:"unknown"` honestly in that case.
//!   * AI-generated only skips at confidence >= 0.7
//!     (`AI_SKIP_CONFIDENCE`); below that the classifier result is
//!     reported but does not skip — a low-confidence AI guess must
//!     not kill a human track.
//!
//! `GEMINI_API_BASE` (default
//! `https://generativelanguage.googleapis.com`) and `GEMINI_MODEL`
//! (default `gemini-2.0-flash`) are overridable so tests point the
//! client at a local stub server — no real key, no network to Google.

use crate::common::*;
use std::process::Command;

/// Confidence at or above which an `ai_generated` verdict skips.
pub const AI_SKIP_CONFIDENCE: f64 = 0.7;

#[derive(Debug, Clone, PartialEq)]
pub enum AiVerdict {
    Clean,
    Profanity { matched: String },
    AiGenerated { confidence: f64 },
    /// No classifier ran and no lyrics exist to judge: honest unknown.
    Unknown { reason: String },
}

#[derive(Debug, Clone)]
pub struct ClassifierResult {
    pub verdict: AiVerdict,
    /// "gemini" | "wordlist" | "wordlist-fallback" | "none"
    pub provider: String,
    /// Free-text reason from the classifier (or why it is unknown).
    pub detail: String,
}

fn gemini_base() -> String {
    std::env::var("GEMINI_API_BASE")
        .ok()
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "https://generativelanguage.googleapis.com".to_string())
}

fn gemini_model() -> String {
    std::env::var("GEMINI_MODEL")
        .ok()
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "gemini-2.0-flash".to_string())
}

fn gemini_key() -> Option<String> {
    std::env::var("GEMINI_API_KEY")
        .ok()
        .filter(|s| !s.is_empty())
}

/// True when a Gemini key is configured (classifier active).
pub fn gemini_configured() -> bool {
    gemini_key().is_some()
}

/// The JSON prompt sent to Gemini. Lyrics are truncated: a full lyric
/// sheet is not needed to judge AI-slop/profanity, and long prompts
/// cost tokens.
fn build_prompt(title: &str, artist: &str, lyrics: Option<&str>) -> String {
    let lyrics_part = match lyrics {
        Some(l) => {
            let mut t = l.chars().take(4000).collect::<String>();
            if l.chars().count() > 4000 {
                t.push_str("\n[truncated]");
            }
            format!("Lyrics:\n{}", t)
        }
        None => "Lyrics: (not available)".to_string(),
    };
    format!(
        "You are a music-lyrics classifier. Classify this track. Reply with JSON only, no other text.\n\nTitle: {}\nArtist: {}\n{}\n\nRules:\n- verdict \"profanity\" if the lyrics or title contain explicit profanity or slurs; set matched_word to the offending word.\n- verdict \"ai_generated\" if the lyrics read like AI-generated song lyrics (generic, repetitive, clich\u{00e9}-stacked phrasing, no specific imagery); set confidence 0.0-1.0.\n- verdict \"clean\" otherwise.\n- If no lyrics are provided, judge only the title/artist and set confidence low (<= 0.5) for ai_generated; never claim ai_generated with high confidence without lyrics.\n\nJSON shape: {{\"verdict\": \"clean\" | \"profanity\" | \"ai_generated\", \"confidence\": 0.0-1.0, \"reason\": \"one sentence\", \"matched_word\": \"word or null\"}}",
        title, artist, lyrics_part
    )
}

/// Parse the Gemini `generateContent` response into a verdict.
/// Returns None when the response is not the expected shape (the
/// caller falls back to the wordlist instead of inventing a verdict).
fn parse_gemini_response(text: &str) -> Option<ClassifierResult> {
    let v = parse_json(text.trim())?;
    let cand = v.get("candidates")?.as_arr()?.first()?;
    let parts = cand.get("content")?.get("parts")?.as_arr()?;
    let raw = parts.first()?.get("text")?.as_str()?;
    // The model may wrap the JSON in ```json fences; strip them.
    let inner = raw
        .trim()
        .trim_start_matches("```json")
        .trim_start_matches("```")
        .trim_end_matches("```")
        .trim();
    let j = parse_json(inner)?;
    let verdict = j.get("verdict")?.as_str()?;
    let confidence = j
        .get("confidence")
        .and_then(|x| match x {
            Json::Num(n) => Some(*n),
            _ => None,
        })
        .unwrap_or(0.0);
    let reason = j
        .get("reason")
        .and_then(|x| x.as_str())
        .unwrap_or("")
        .to_string();
    let matched = j
        .get("matched_word")
        .and_then(|x| x.as_str())
        .unwrap_or("")
        .to_string();
    let verdict = match verdict {
        "profanity" => AiVerdict::Profanity {
            matched: if matched.is_empty() {
                "unknown".to_string()
            } else {
                matched
            },
        },
        "ai_generated" => AiVerdict::AiGenerated { confidence },
        _ => AiVerdict::Clean,
    };
    Some(ClassifierResult {
        verdict,
        provider: "gemini".to_string(),
        detail: reason,
    })
}

/// One Gemini call. `api_key` is passed as a header, never logged.
/// Returns None on any transport / HTTP / parse failure.
pub fn classify_with_gemini(
    api_base: &str,
    model: &str,
    api_key: &str,
    title: &str,
    artist: &str,
    lyrics: Option<&str>,
) -> Option<ClassifierResult> {
    let url = format!(
        "{}/v1beta/models/{}:generateContent",
        api_base.trim_end_matches('/'),
        model
    );
    let prompt = build_prompt(title, artist, lyrics);
    let body = Json::Obj(vec![
        (
            "contents".to_string(),
            Json::Arr(vec![Json::Obj(vec![(
                "parts".to_string(),
                Json::Arr(vec![Json::Obj(vec![(
                    "text".to_string(),
                    Json::Str(prompt),
                )])]),
            )])]),
        ),
        (
            "generationConfig".to_string(),
            Json::Obj(vec![
                ("responseMimeType".to_string(), Json::Str("application/json".to_string())),
                ("temperature".to_string(), Json::Num(0.0)),
            ]),
        ),
    ])
    .render();
    let out = Command::new("curl")
        .arg("-sS")
        .arg("-f")
        .arg("--max-time")
        .arg("30")
        .arg("-H")
        .arg("Content-Type: application/json")
        .arg("-H")
        .arg(format!("x-goog-api-key: {}", api_key))
        .arg("-d")
        .arg(&body)
        .arg(&url)
        .output()
        .ok()?;
    if !out.status.success() {
        eprintln!(
            "[spotify-rust] Gemini classify failed: {}",
            String::from_utf8_lossy(&out.stderr).trim()
        );
        return None;
    }
    let text = String::from_utf8_lossy(&out.stdout);
    match parse_gemini_response(&text) {
        Some(r) => Some(r),
        None => {
            eprintln!("[spotify-rust] Gemini response unparsable, falling back to wordlist");
            None
        }
    }
}

/// Classify one track's text. `profanity_check` is the wordlist
/// function from main.rs, passed in so this module stays testable
/// without the profanity list.
///
/// Order: Gemini (key set) -> wordlist fallback (Gemini failed) ->
/// wordlist only (no key). Without a key AND without lyrics, the AI
/// verdict is honestly `Unknown`.
pub fn classify_track(
    title: &str,
    artist: &str,
    lyrics: Option<&str>,
    profanity_check: &dyn Fn(&str) -> Option<String>,
) -> ClassifierResult {
    let text = format!("{} {}", title, artist);
    if let Some(key) = gemini_key() {
        let base = gemini_base();
        let model = gemini_model();
        if let Some(r) = classify_with_gemini(&base, &model, &key, title, artist, lyrics) {
            return r;
        }
        // Gemini failed: fall back to the wordlist, and say so.
        return wordlist_result(&text, lyrics, profanity_check, "wordlist-fallback");
    }
    if lyrics.is_none() {
        // No classifier, no lyrics: profanity can still be judged on
        // title/artist, but AI-ness cannot — report unknown honestly.
        if let Some(word) = profanity_check(&text) {
            return ClassifierResult {
                verdict: AiVerdict::Profanity { matched: word },
                provider: "wordlist".to_string(),
                detail: "profanity in title/artist (wordlist); no lyrics and no Gemini key, so AI verdict is unknown".to_string(),
            };
        }
        return ClassifierResult {
            verdict: AiVerdict::Unknown {
                reason: "no GEMINI_API_KEY and no lyrics: cannot judge AI-generated".to_string(),
            },
            provider: "none".to_string(),
            detail: "no classifier configured and no lyrics fetched".to_string(),
        };
    }
    wordlist_result(&text, lyrics, profanity_check, "wordlist")
}

fn wordlist_result(
    text: &str,
    lyrics: Option<&str>,
    profanity_check: &dyn Fn(&str) -> Option<String>,
    provider: &str,
) -> ClassifierResult {
    if let Some(word) = profanity_check(text) {
        return ClassifierResult {
            verdict: AiVerdict::Profanity { matched: word },
            provider: provider.to_string(),
            detail: "profanity in title/artist (wordlist)".to_string(),
        };
    }
    if let Some(lyr) = lyrics {
        if let Some(word) = profanity_check(lyr) {
            return ClassifierResult {
                verdict: AiVerdict::Profanity { matched: word },
                provider: provider.to_string(),
                detail: "profanity in lyrics (wordlist)".to_string(),
            };
        }
    }
    ClassifierResult {
        verdict: AiVerdict::Clean,
        provider: provider.to_string(),
        detail: "no profanity found (wordlist)".to_string(),
    }
}

/// The verdict string reported in skip-check responses.
pub fn verdict_name(v: &AiVerdict) -> &'static str {
    match v {
        AiVerdict::Clean => "clean",
        AiVerdict::Profanity { .. } => "profanity",
        AiVerdict::AiGenerated { .. } => "ai_generated",
        AiVerdict::Unknown { .. } => "unknown",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Env-mutating tests share this lock: Rust runs tests in parallel
    // in one process, and GEMINI_API_KEY is process-global.
    static ENV_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    // Reuses the stub-server shape from auth.rs tests via a local
    // copy (each module's tests are self-contained).
    fn stub_server(responses: Vec<(u16, String)>) -> (String, std::thread::JoinHandle<()>) {
        use std::io::{Read, Write};
        use std::net::TcpListener;
        use std::sync::mpsc;
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind stub");
        let port = listener.local_addr().unwrap().port();
        let (tx, rx) = mpsc::channel::<()>();
        let handle = std::thread::spawn(move || {
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
                let reason = if status == 200 { "OK" } else { "Error" };
                let resp = format!(
                    "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                    status, reason, body.len(), body
                );
                let _ = stream.write_all(resp.as_bytes());
                let _ = stream.flush();
            }
        });
        let _ = rx.recv();
        (format!("http://127.0.0.1:{}", port), handle)
    }

    fn gemini_ok_response(verdict: &str, confidence: f64, reason: &str) -> String {
        let inner = format!(
            "{{\"verdict\": {}, \"confidence\": {}, \"reason\": {}, \"matched_word\": null}}",
            quote(verdict),
            confidence,
            quote(reason)
        );
        // Gemini wraps the JSON text inside candidates[0].content.parts[0].text.
        let outer = Json::Obj(vec![(
            "candidates".to_string(),
            Json::Arr(vec![Json::Obj(vec![(
                "content".to_string(),
                Json::Obj(vec![(
                    "parts".to_string(),
                    Json::Arr(vec![Json::Obj(vec![(
                        "text".to_string(),
                        Json::Str(inner),
                    )])]),
                )]),
            )])]),
        )])
        .render();
        outer
    }

    #[test]
    fn parse_gemini_ai_generated() {
        let raw = gemini_ok_response("ai_generated", 0.92, "generic repetitive phrasing");
        let r = parse_gemini_response(&raw).expect("parses");
        assert_eq!(r.provider, "gemini");
        assert_eq!(r.verdict, AiVerdict::AiGenerated { confidence: 0.92 });
    }

    #[test]
    fn parse_gemini_profanity_and_fences() {
        // Wrap the inner text in ```json fences like the model sometimes does.
        let fenced = format!(
            "{{\"candidates\":[{{\"content\":{{\"parts\":[{{\"text\": \"```json\\n{{\\\"verdict\\\": \\\"profanity\\\", \\\"confidence\\\": 0.99, \\\"reason\\\": \\\"explicit\\\", \\\"matched_word\\\": \\\"shit\\\"}}\\n```\"}}]}}}}]}}"
        );
        let r = parse_gemini_response(&fenced).expect("parses fenced JSON");
        assert_eq!(
            r.verdict,
            AiVerdict::Profanity {
                matched: "shit".to_string()
            }
        );
    }

    #[test]
    fn parse_gemini_garbage_is_none() {
        assert!(parse_gemini_response("not json").is_none());
        assert!(parse_gemini_response(r#"{"candidates":[]}"#).is_none());
        assert!(parse_gemini_response(
            r#"{"candidates":[{"content":{"parts":[{"text":"hello"}]}}]}"#
        )
        .is_none());
    }

    #[test]
    fn gemini_call_against_stub() {
        let (base, handle) = stub_server(vec![(
            200,
            gemini_ok_response("ai_generated", 0.88, "reads AI-written"),
        )]);
        let r = classify_with_gemini(
            &base,
            "gemini-2.0-flash",
            "test-key-never-real",
            "Neon Skyline",
            "Synth Ghost",
            Some("we dance in the neon light, we dance all night"),
        )
        .expect("stub answers");
        assert_eq!(r.verdict, AiVerdict::AiGenerated { confidence: 0.88 });
        handle.join().unwrap();
    }

    #[test]
    fn gemini_http_error_is_none() {
        let (base, handle) = stub_server(vec![(400, r#"{"error":{"message":"bad key"}}"#.to_string())]);
        assert!(classify_with_gemini(&base, "gemini-2.0-flash", "bad-key", "T", "A", None).is_none());
        handle.join().unwrap();
    }

    #[test]
    fn classify_without_key_no_lyrics_is_unknown() {
        let _guard = ENV_LOCK.lock().unwrap();
        let had = std::env::var("GEMINI_API_KEY").ok();
        std::env::remove_var("GEMINI_API_KEY");
        let no_profanity = |_: &str| None;
        let r = classify_track("Clean Title", "Clean Artist", None, &no_profanity);
        assert_eq!(
            r.verdict,
            AiVerdict::Unknown {
                reason: "no GEMINI_API_KEY and no lyrics: cannot judge AI-generated".to_string()
            }
        );
        assert_eq!(r.provider, "none");
        if let Some(k) = had {
            std::env::set_var("GEMINI_API_KEY", k);
        }
    }

    #[test]
    fn classify_without_key_wordlist_profanity() {
        let _guard = ENV_LOCK.lock().unwrap();
        let had = std::env::var("GEMINI_API_KEY").ok();
        std::env::remove_var("GEMINI_API_KEY");
        let check = |t: &str| {
            if t.to_lowercase().contains("shit") {
                Some("shit".to_string())
            } else {
                None
            }
        };
        let r = classify_track("Shit Song", "Artist", None, &check);
        assert_eq!(
            r.verdict,
            AiVerdict::Profanity {
                matched: "shit".to_string()
            }
        );
        assert_eq!(r.provider, "wordlist");
        if let Some(k) = had {
            std::env::set_var("GEMINI_API_KEY", k);
        }
    }
}
