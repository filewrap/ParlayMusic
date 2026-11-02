#![allow(dead_code)]
// Shared std-only helpers for Parlay Rust services: minimal JSON parser/
// serializer, tiny HTTP/1.1 server, auth check, atomic file writes,
// /dev/urandom token generation. No external crates.
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::Arc;

// ---------------------------------------------------------------- JSON ---

#[derive(Debug, Clone, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    Num(f64),
    Str(String),
    Arr(Vec<Json>),
    Obj(Vec<(String, Json)>),
}

impl Json {
    pub fn get(&self, key: &str) -> Option<&Json> {
        if let Json::Obj(pairs) = self {
            pairs.iter().find(|(k, _)| k == key).map(|(_, v)| v)
        } else {
            None
        }
    }
    pub fn as_str(&self) -> Option<&str> {
        if let Json::Str(s) = self {
            Some(s)
        } else {
            None
        }
    }
    pub fn as_i64(&self) -> Option<i64> {
        if let Json::Num(n) = self {
            Some(*n as i64)
        } else {
            None
        }
    }
    pub fn as_bool(&self) -> Option<bool> {
        if let Json::Bool(b) = self {
            Some(*b)
        } else {
            None
        }
    }
    pub fn render(&self) -> String {
        match self {
            Json::Null => "null".to_string(),
            Json::Bool(b) => b.to_string(),
            Json::Num(n) => {
                if n.fract() == 0.0 && n.abs() < 9.0e15 {
                    format!("{}", *n as i64)
                } else {
                    format!("{}", n)
                }
            }
            Json::Str(s) => quote(s),
            Json::Arr(a) => format!(
                "[{}]",
                a.iter().map(|v| v.render()).collect::<Vec<_>>().join(",")
            ),
            Json::Obj(o) => format!(
                "{{{}}}",
                o.iter()
                    .map(|(k, v)| format!("{}:{}", quote(k), v.render()))
                    .collect::<Vec<_>>()
                    .join(",")
            ),
        }
    }
}

pub fn esc(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for ch in s.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out
}

pub fn quote(s: &str) -> String {
    format!("\"{}\"", esc(s))
}

struct Parser {
    c: Vec<char>,
    i: usize,
}

impl Parser {
    fn ws(&mut self) {
        while self.i < self.c.len() && self.c[self.i].is_whitespace() {
            self.i += 1;
        }
    }
    fn peek(&self) -> Option<char> {
        self.c.get(self.i).copied()
    }
    fn val(&mut self) -> Option<Json> {
        self.ws();
        match self.peek()? {
            '{' => self.obj(),
            '[' => self.arr(),
            '"' => self.string().map(Json::Str),
            't' => self.lit("true", Json::Bool(true)),
            'f' => self.lit("false", Json::Bool(false)),
            'n' => self.lit("null", Json::Null),
            _ => self.num(),
        }
    }
    fn lit(&mut self, s: &str, v: Json) -> Option<Json> {
        for ch in s.chars() {
            if self.peek()? != ch {
                return None;
            }
            self.i += 1;
        }
        Some(v)
    }
    fn num(&mut self) -> Option<Json> {
        let start = self.i;
        if self.peek() == Some('-') {
            self.i += 1;
        }
        while matches!(self.peek(), Some(c) if c.is_ascii_digit() || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
            self.i += 1;
        }
        if self.i == start {
            return None;
        }
        let s: String = self.c[start..self.i].iter().collect();
        s.parse::<f64>().ok().map(Json::Num)
    }
    fn string(&mut self) -> Option<String> {
        if self.peek()? != '"' {
            return None;
        }
        self.i += 1;
        let mut out = String::new();
        while let Some(ch) = self.peek() {
            self.i += 1;
            match ch {
                '"' => return Some(out),
                '\\' => match self.peek()? {
                    '"' => {
                        out.push('"');
                        self.i += 1;
                    }
                    '\\' => {
                        out.push('\\');
                        self.i += 1;
                    }
                    '/' => {
                        out.push('/');
                        self.i += 1;
                    }
                    'n' => {
                        out.push('\n');
                        self.i += 1;
                    }
                    't' => {
                        out.push('\t');
                        self.i += 1;
                    }
                    'r' => {
                        out.push('\r');
                        self.i += 1;
                    }
                    'b' => {
                        out.push('\u{0008}');
                        self.i += 1;
                    }
                    'f' => {
                        out.push('\u{000C}');
                        self.i += 1;
                    }
                    'u' => {
                        self.i += 1;
                        let mut code = 0u32;
                        for _ in 0..4 {
                            let d = self.peek()?.to_digit(16)?;
                            code = code * 16 + d;
                            self.i += 1;
                        }
                        out.push(char::from_u32(code).unwrap_or('\u{FFFD}'));
                    }
                    _ => return None,
                },
                _ => out.push(ch),
            }
        }
        None
    }
    fn arr(&mut self) -> Option<Json> {
        self.i += 1; // [
        let mut items = Vec::new();
        self.ws();
        if self.peek() == Some(']') {
            self.i += 1;
            return Some(Json::Arr(items));
        }
        loop {
            items.push(self.val()?);
            self.ws();
            match self.peek()? {
                ',' => {
                    self.i += 1;
                }
                ']' => {
                    self.i += 1;
                    return Some(Json::Arr(items));
                }
                _ => return None,
            }
        }
    }
    fn obj(&mut self) -> Option<Json> {
        self.i += 1; // {
        let mut pairs = Vec::new();
        self.ws();
        if self.peek() == Some('}') {
            self.i += 1;
            return Some(Json::Obj(pairs));
        }
        loop {
            self.ws();
            let k = self.string()?;
            self.ws();
            if self.peek()? != ':' {
                return None;
            }
            self.i += 1;
            let v = self.val()?;
            pairs.push((k, v));
            self.ws();
            match self.peek()? {
                ',' => {
                    self.i += 1;
                }
                '}' => {
                    self.i += 1;
                    return Some(Json::Obj(pairs));
                }
                _ => return None,
            }
        }
    }
}

pub fn parse_json(s: &str) -> Option<Json> {
    let mut p = Parser {
        c: s.chars().collect(),
        i: 0,
    };
    let v = p.val()?;
    p.ws();
    if p.i != p.c.len() {
        return None;
    }
    Some(v)
}

// ---------------------------------------------------------------- HTTP ---

pub struct Request {
    pub method: String,
    pub path: String,
    pub headers: Vec<(String, String)>,
    pub body: String,
}

impl Request {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers
            .iter()
            .find(|(k, _)| k.eq_ignore_ascii_case(name))
            .map(|(_, v)| v.as_str())
    }
}

fn find_subslice(hay: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() || hay.len() < needle.len() {
        return None;
    }
    hay.windows(needle.len()).position(|w| w == needle)
}

pub fn read_request(stream: &mut TcpStream) -> Option<Request> {
    let mut buf: Vec<u8> = Vec::new();
    let mut tmp = [0u8; 4096];
    loop {
        let n = stream.read(&mut tmp).ok()?;
        if n == 0 {
            return None;
        }
        buf.extend_from_slice(&tmp[..n]);
        if find_subslice(&buf, b"\r\n\r\n").is_some() {
            break;
        }
        if buf.len() > 65536 {
            return None;
        }
    }
    let head_end = find_subslice(&buf, b"\r\n\r\n")? + 4;
    let head = String::from_utf8_lossy(&buf[..head_end]).into_owned();
    let mut lines = head.lines();
    let first = lines.next()?;
    let mut parts = first.split_whitespace();
    let method = parts.next()?.to_string();
    let raw_path = parts.next().unwrap_or("/").to_string();
    let path = raw_path
        .split('?')
        .next()
        .unwrap_or("/")
        .to_string();
    let mut headers: Vec<(String, String)> = Vec::new();
    let mut content_length = 0usize;
    for line in lines {
        if let Some((k, v)) = line.split_once(':') {
            let k = k.trim().to_string();
            let v = v.trim().to_string();
            if k.eq_ignore_ascii_case("content-length") {
                content_length = v.parse().unwrap_or(0);
            }
            headers.push((k, v));
        }
    }
    let mut body = buf[head_end..].to_vec();
    while body.len() < content_length {
        let n = stream.read(&mut tmp).ok()?;
        if n == 0 {
            break;
        }
        body.extend_from_slice(&tmp[..n]);
    }
    body.truncate(content_length);
    Some(Request {
        method,
        path,
        headers,
        body: String::from_utf8_lossy(&body).into_owned(),
    })
}

pub fn respond(stream: &mut TcpStream, status: u16, body: &str) {
    let reason = match status {
        200 => "OK",
        400 => "Bad Request",
        401 => "Unauthorized",
        404 => "Not Found",
        405 => "Method Not Allowed",
        500 => "Internal Server Error",
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

/// Success envelope per contracts/ipc.json: {"ok":true,"data":{...}}
pub fn ok(data_json: &str) -> String {
    format!("{{\"ok\":true,\"data\":{}}}", data_json)
}

/// Error envelope: {"ok":false,"error":{"code":...,"message":...}}
pub fn err(code: &str, message: &str) -> String {
    format!(
        "{{\"ok\":false,\"error\":{{\"code\":{},\"message\":{}}}}}",
        quote(code),
        quote(message)
    )
}

/// If $PARLAY_TOKEN is set and non-empty, the request must carry a matching
/// X-Parlay-Token header. Unset token = no auth (dev default).
pub fn auth_ok(req: &Request) -> bool {
    match std::env::var("PARLAY_TOKEN") {
        Ok(t) if !t.is_empty() => req
            .header("X-Parlay-Token")
            .map(|v| v == t)
            .unwrap_or(false),
        _ => true,
    }
}

pub fn run_server<F>(port: u16, service: &'static str, handler: F) -> std::io::Result<()>
where
    F: Fn(&Request) -> (u16, String) + Send + Sync + 'static,
{
    let listener = TcpListener::bind(("0.0.0.0", port))?;
    eprintln!("parlay {} listening on 0.0.0.0:{}", service, port);
    let handler = Arc::new(handler);
    for stream in listener.incoming() {
        match stream {
            Ok(mut s) => {
                let h = Arc::clone(&handler);
                std::thread::spawn(move || {
                    let _ = s.set_read_timeout(Some(std::time::Duration::from_secs(30)));
                    if let Some(req) = read_request(&mut s) {
                        if req.method == "GET" && req.path == "/healthz" {
                            let body = format!(
                                "{{\"ok\":true,\"service\":{},\"data\":{{\"service\":{},\"status\":\"ok\"}}}}",
                                quote(service),
                                quote(service)
                            );
                            respond(&mut s, 200, &body);
                            return;
                        }
                        if !auth_ok(&req) {
                            respond(
                                &mut s,
                                401,
                                &err("unauthorized", "missing or invalid X-Parlay-Token"),
                            );
                            return;
                        }
                        let (status, body) = h(&req);
                        respond(&mut s, status, &body);
                    }
                });
            }
            Err(e) => eprintln!("accept error: {}", e),
        }
    }
    Ok(())
}

// -------------------------------------------------------------- helpers ---

/// Atomic write: write temp file in the same dir, then rename over target.
pub fn atomic_write(path: &std::path::Path, content: &str) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            std::fs::create_dir_all(parent)?;
        }
    }
    let tmp = path.with_extension("tmp");
    std::fs::write(&tmp, content)?;
    std::fs::rename(&tmp, path)?;
    Ok(())
}

/// Random alphanumeric token of `len` chars from /dev/urandom
/// (xorshift/time fallback only if urandom is unavailable).
pub fn random_token(len: usize) -> String {
    const ALPH: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    let mut bytes = vec![0u8; len];
    let got_urandom = std::fs::File::open("/dev/urandom")
        .and_then(|mut f| f.read_exact(&mut bytes))
        .is_ok();
    if !got_urandom {
        let seed = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos() as u64)
            .unwrap_or(0)
            ^ ((std::process::id() as u64) << 32);
        let mut x = seed | 1;
        for b in bytes.iter_mut() {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            *b = (x & 0xff) as u8;
        }
    }
    bytes
        .iter()
        .map(|b| ALPH[(*b as usize) % ALPH.len()] as char)
        .collect()
}

/// Data file path: $PARLAY_DATA_DIR/<file> or ./data/<file>.
pub fn data_file(name: &str) -> std::path::PathBuf {
    let dir = std::env::var("PARLAY_DATA_DIR").unwrap_or_else(|_| "data".to_string());
    std::path::Path::new(&dir).join(name)
}
