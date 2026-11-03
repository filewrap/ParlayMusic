// store.rs — Parlay shared durable store (std-only, no crates).
//
// One shared store for ALL stateful Parlay services, so a queue written
// by one VC engine is readable by another after fail-over. Backend
// priority is uniform everywhere (same as core-cpp `Store`):
//
//   MONGO_URI set -> MongoDB, REDIS_URL set -> Redis, else JSON file.
//
// The JSON-file path is NOT implemented here: when neither variable is
// set this module reports backend "file" and the service keeps its
// existing file code byte-for-byte (no regression). A configured
// backend that cannot be reached is FATAL at startup (init returns
// Err; the service exits) — silently falling back to a local file
// while another engine writes to Mongo/Redis would split-brain the
// queue. (core-cpp itself falls through with a warning; its store is
// A7-owned and off-limits here, and its keys are core-internal, so the
// divergence is safe and documented in phase7/worker-a11-store.md.)
//
// Wire protocols are hand-rolled std-only ports of core-cpp
// `src/store.cpp` (OP_MSG + minimal BSON; RESP), so builds stay
// offline-capable: no new crates. Mongo SCRAM auth IS implemented
// here (SCRAM-SHA-256, falling back to SCRAM-SHA-1 — the pure-RFC
// client in the `scram` section below, ported from core-cpp
// `src/scram.{hpp,cpp}`): URIs with credentials authenticate
// against the authSource database (default: the URI database, else
// "admin"); `?authMechanism=` forces one mechanism. Compose Mongo
// has no auth, so this only matters for auth-required deployments.
//
// Key scheme (canonical, all services — matches core-cpp exactly):
//   Mongo: db from URI (default `parlay`), collection `state`,
//          document {_id: "kv:<key>", v: "<string value>"}.
//   Redis: key "parlay:kv:<key>" -> "<string value>".
//   Keys: vc:room:<chat_id>  canonical VC room JSON (see
//          docs in ARCHITECTURE.md, "Shared durable store")
//          queue:votes       queue-rust votes map JSON
//          share:links       share-rust token -> track map JSON
//          spotify:state     spotify-rust state JSON
//          radio:state       radio-js {radios:[...]} JSON
//          <core keys>       core-cpp internal keys (unchanged)
//
// NOTE: this file is copied verbatim into vc-rust, queue-rust,
// share-rust and spotify-rust (separate crates, no workspace). Keep
// the copies in sync; it deals only in String values so it does not
// depend on any service's Json type.

use std::io::{Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::sync::Mutex;
use std::time::Duration;

const IO_TIMEOUT: Duration = Duration::from_secs(5);
const MONGO_COLLECTION: &str = "state";

// ------------------------------------------------------------------ util --

fn strip_credentials(uri: &str) -> String {
    // For log lines: drop any userinfo, keep scheme://host... .
    if let Some(at) = uri.rfind('@') {
        if let Some(scheme) = uri.find("://") {
            return format!("{}<credentials>@{}", &uri[..scheme + 3], &uri[at + 1..]);
        }
    }
    uri.to_string()
}

fn pct_decode(input: &str) -> Option<String> {
    let mut out = String::new();
    let mut chars = input.chars();
    while let Some(c) = chars.next() {
        if c != '%' {
            out.push(c);
            continue;
        }
        let hi = chars.next()?.to_digit(16)?;
        let lo = chars.next()?.to_digit(16)?;
        out.push(char::from(((hi << 4) | lo) as u8));
    }
    Some(out)
}

// ----------------------------------------------------------------- SCRAM --
// Self-contained SHA-1 / SHA-256 / MD5 / HMAC / PBKDF2 / base64 and
// the SCRAM client side (RFC 5802 for SHA-1, RFC 7677 for SHA-256),
// ported from core-cpp `src/scram.{hpp,cpp}` (same vectors, same
// message shapes). Std-only: no new crates, offline builds keep
// working. Passwords are used as UTF-8 bytes with no SASLprep
// normalization (RFC 4013) — exact for ASCII passwords, the
// realistic case for MONGO_URI (same limitation as core-cpp).

fn sha256(data: &[u8]) -> Vec<u8> {
    const K: [u32; 64] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    ];
    let rotr = |x: u32, n: u32| (x >> n) | (x << (32 - n));
    let mut h: [u32; 8] = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c,
        0x1f83d9ab, 0x5be0cd19,
    ];
    let mut msg = data.to_vec();
    let bit_len = (data.len() as u64) * 8;
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&bit_len.to_be_bytes());
    for off in (0..msg.len()).step_by(64) {
        let mut w = [0u32; 64];
        for i in 0..16 {
            w[i] = u32::from_be_bytes([
                msg[off + i * 4],
                msg[off + i * 4 + 1],
                msg[off + i * 4 + 2],
                msg[off + i * 4 + 3],
            ]);
        }
        for i in 16..64 {
            let s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            let s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16]
                .wrapping_add(s0)
                .wrapping_add(w[i - 7])
                .wrapping_add(s1);
        }
        let (mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut hh) =
            (h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
        for i in 0..64 {
            let s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            let ch = (e & f) ^ ((!e) & g);
            let t1 = hh
                .wrapping_add(s1)
                .wrapping_add(ch)
                .wrapping_add(K[i])
                .wrapping_add(w[i]);
            let s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            let maj = (a & b) ^ (a & c) ^ (b & c);
            let t2 = s0.wrapping_add(maj);
            hh = g;
            g = f;
            f = e;
            e = d.wrapping_add(t1);
            d = c;
            c = b;
            b = a;
            a = t1.wrapping_add(t2);
        }
        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e);
        h[5] = h[5].wrapping_add(f);
        h[6] = h[6].wrapping_add(g);
        h[7] = h[7].wrapping_add(hh);
    }
    let mut out = Vec::with_capacity(32);
    for v in h {
        out.extend_from_slice(&v.to_be_bytes());
    }
    out
}

fn sha1(data: &[u8]) -> Vec<u8> {
    let rotl = |x: u32, n: u32| (x << n) | (x >> (32 - n));
    let (mut h0, mut h1, mut h2, mut h3, mut h4) =
        (0x67452301u32, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0);
    let mut msg = data.to_vec();
    let bit_len = (data.len() as u64) * 8;
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&bit_len.to_be_bytes());
    for off in (0..msg.len()).step_by(64) {
        let mut w = [0u32; 80];
        for i in 0..16 {
            w[i] = u32::from_be_bytes([
                msg[off + i * 4],
                msg[off + i * 4 + 1],
                msg[off + i * 4 + 2],
                msg[off + i * 4 + 3],
            ]);
        }
        for i in 16..80 {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        let (mut a, mut b, mut c, mut d, mut e) = (h0, h1, h2, h3, h4);
        for i in 0..80 {
            let (f, k) = if i < 20 {
                ((b & c) | ((!b) & d), 0x5A827999u32)
            } else if i < 40 {
                (b ^ c ^ d, 0x6ED9EBA1)
            } else if i < 60 {
                ((b & c) | (b & d) | (c & d), 0x8F1BBCDC)
            } else {
                (b ^ c ^ d, 0xCA62C1D6)
            };
            let tmp = rotl(a, 5)
                .wrapping_add(f)
                .wrapping_add(e)
                .wrapping_add(k)
                .wrapping_add(w[i]);
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = tmp;
        }
        h0 = h0.wrapping_add(a);
        h1 = h1.wrapping_add(b);
        h2 = h2.wrapping_add(c);
        h3 = h3.wrapping_add(d);
        h4 = h4.wrapping_add(e);
    }
    let mut out = Vec::with_capacity(20);
    for v in [h0, h1, h2, h3, h4] {
        out.extend_from_slice(&v.to_be_bytes());
    }
    out
}

fn md5(data: &[u8]) -> Vec<u8> {
    // RFC 1321. Only used for MongoDB's SCRAM-SHA-1 password
    // derivation (mongo_sha1_password, below).
    const K: [u32; 64] = [
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
        0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
        0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
        0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    ];
    const S: [u32; 64] = [
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14,
        20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11,
        16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21,
        6, 10, 15, 21, 6, 10, 15, 21,
    ];
    let lrot = |x: u32, c: u32| (x << c) | (x >> (32 - c));
    let mut msg = data.to_vec();
    let bit_len = (data.len() as u64) * 8;
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&bit_len.to_le_bytes());
    let (mut a0, mut b0, mut c0, mut d0) =
        (0x67452301u32, 0xefcdab89, 0x98badcfe, 0x10325476);
    for off in (0..msg.len()).step_by(64) {
        let mut m = [0u32; 16];
        for i in 0..16 {
            m[i] = u32::from_le_bytes([
                msg[off + i * 4],
                msg[off + i * 4 + 1],
                msg[off + i * 4 + 2],
                msg[off + i * 4 + 3],
            ]);
        }
        let (mut a, mut b, mut c, mut d) = (a0, b0, c0, d0);
        for i in 0..64usize {
            let (f, g): (u32, usize) = if i < 16 {
                ((b & c) | ((!b) & d), i)
            } else if i < 32 {
                ((d & b) | ((!d) & c), (5 * i + 1) % 16)
            } else if i < 48 {
                (b ^ c ^ d, (3 * i + 5) % 16)
            } else {
                (c ^ (b | (!d)), (7 * i) % 16)
            };
            let f = f
                .wrapping_add(a)
                .wrapping_add(K[i])
                .wrapping_add(m[g]);
            a = d;
            d = c;
            c = b;
            b = b.wrapping_add(lrot(f, S[i]));
        }
        a0 = a0.wrapping_add(a);
        b0 = b0.wrapping_add(b);
        c0 = c0.wrapping_add(c);
        d0 = d0.wrapping_add(d);
    }
    let mut out = Vec::with_capacity(16);
    for v in [a0, b0, c0, d0] {
        out.extend_from_slice(&v.to_le_bytes());
    }
    out
}

fn hex_encode(raw: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(raw.len() * 2);
    for &b in raw {
        out.push(HEX[(b >> 4) as usize] as char);
        out.push(HEX[(b & 0xf) as usize] as char);
    }
    out
}

fn hmac(hash_fn: fn(&[u8]) -> Vec<u8>, key: &[u8], msg: &[u8]) -> Vec<u8> {
    // RFC 2104. block_size 64 for both SHA-1 and SHA-256.
    let mut k = key.to_vec();
    if k.len() > 64 {
        k = hash_fn(&k);
    }
    k.resize(64, 0);
    let mut ipad = vec![0x36u8; 64];
    let mut opad = vec![0x5cu8; 64];
    for i in 0..64 {
        ipad[i] ^= k[i];
        opad[i] ^= k[i];
    }
    let mut inner = ipad;
    inner.extend_from_slice(msg);
    let inner_hash = hash_fn(&inner);
    let mut outer = opad;
    outer.extend_from_slice(&inner_hash);
    hash_fn(&outer)
}

fn hmac_sha1(key: &[u8], msg: &[u8]) -> Vec<u8> {
    hmac(sha1, key, msg)
}

fn hmac_sha256(key: &[u8], msg: &[u8]) -> Vec<u8> {
    hmac(sha256, key, msg)
}

fn pbkdf2(
    hmac_fn: fn(&[u8], &[u8]) -> Vec<u8>,
    password: &[u8],
    salt: &[u8],
    iterations: u32,
    dk_len: usize,
) -> Vec<u8> {
    // RFC 2898. hash_len is the HMAC output size.
    let hash_len = hmac_fn(password, b"").len();
    let mut out = Vec::new();
    let mut block: u32 = 1;
    while out.len() < dk_len {
        let mut salt_block = salt.to_vec();
        salt_block.extend_from_slice(&block.to_be_bytes());
        let mut u = hmac_fn(password, &salt_block);
        let mut t = u.clone();
        for _ in 1..iterations {
            u = hmac_fn(password, &u);
            for j in 0..hash_len {
                t[j] ^= u[j];
            }
        }
        out.extend_from_slice(&t);
        block += 1;
    }
    out.truncate(dk_len);
    out
}

fn pbkdf2_sha1(password: &[u8], salt: &[u8], iterations: u32, dk_len: usize) -> Vec<u8> {
    pbkdf2(hmac_sha1, password, salt, iterations, dk_len)
}

fn pbkdf2_sha256(password: &[u8], salt: &[u8], iterations: u32, dk_len: usize) -> Vec<u8> {
    pbkdf2(hmac_sha256, password, salt, iterations, dk_len)
}

fn base64_encode(data: &[u8]) -> String {
    const ALPHABET: &[u8; 64] =
        b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::new();
    for chunk in data.chunks(3) {
        let n = ((chunk[0] as u32) << 16)
            | (if chunk.len() > 1 { (chunk[1] as u32) << 8 } else { 0 })
            | (if chunk.len() > 2 { chunk[2] as u32 } else { 0 });
        out.push(ALPHABET[((n >> 18) & 63) as usize] as char);
        out.push(ALPHABET[((n >> 12) & 63) as usize] as char);
        out.push(if chunk.len() > 1 {
            ALPHABET[((n >> 6) & 63) as usize] as char
        } else {
            '='
        });
        out.push(if chunk.len() > 2 {
            ALPHABET[(n & 63) as usize] as char
        } else {
            '='
        });
    }
    out
}

fn base64_decode(input: &str) -> Option<Vec<u8>> {
    let val = |c: u8| -> Option<u32> {
        match c {
            b'A'..=b'Z' => Some((c - b'A') as u32),
            b'a'..=b'z' => Some((c - b'a' + 26) as u32),
            b'0'..=b'9' => Some((c - b'0' + 52) as u32),
            b'+' => Some(62),
            b'/' => Some(63),
            _ => None,
        }
    };
    let bytes = input.as_bytes();
    if bytes.len() % 4 != 0 {
        return None;
    }
    let mut out = Vec::new();
    for chunk in bytes.chunks(4) {
        let mut pad = 0;
        if chunk[3] == b'=' {
            pad += 1;
        }
        if chunk[2] == b'=' {
            pad += 1;
        }
        let mut v = [0u32; 4];
        for (j, &c) in chunk.iter().enumerate() {
            if c == b'=' {
                v[j] = 0;
            } else {
                v[j] = val(c)?;
            }
        }
        let n = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
        out.push(((n >> 16) & 0xff) as u8);
        if pad < 2 {
            out.push(((n >> 8) & 0xff) as u8);
        }
        if pad < 1 {
            out.push((n & 0xff) as u8);
        }
    }
    Some(out)
}

/// MongoDB's SCRAM-SHA-1 password derivation: the server stores
/// SCRAM-SHA-1 credentials derived from hex(MD5(username +
/// ":mongo:" + password)) — the legacy MONGODB-CR digest — NOT from
/// the raw password (verified live against mongod 8.0.4 by A7: the
/// raw password fails, this digest authenticates). SCRAM-SHA-256
/// uses the raw password.
fn mongo_sha1_password(username: &str, password: &str) -> String {
    let digest_input = format!("{}:mongo:{}", username, password);
    hex_encode(&md5(digest_input.as_bytes()))
}

fn random_nonce_base64(n_bytes: usize) -> String {
    // 18 random bytes from /dev/urandom, base64. Falls back to a
    // time+pid+counter hash when urandom is unavailable — a nonce
    // must never repeat across attempts.
    use std::io::Read;
    let mut raw = vec![0u8; n_bytes];
    let mut ok = false;
    if let Ok(mut f) = std::fs::File::open("/dev/urandom") {
        if f.read_exact(&mut raw).is_ok() {
            ok = true;
        }
    }
    if !ok {
        static COUNTER: std::sync::atomic::AtomicU64 =
            std::sync::atomic::AtomicU64::new(0);
        let seed = format!(
            "{}{}{}",
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_nanos())
                .unwrap_or(0),
            std::process::id(),
            COUNTER.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        );
        let hash = sha256(seed.as_bytes());
        for i in 0..n_bytes {
            raw[i] = hash[i % hash.len()] ^ ((i * 31) as u8);
        }
    }
    base64_encode(&raw)
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum ScramMechanism {
    Sha1,
    Sha256,
}

fn mechanism_name(m: ScramMechanism) -> &'static str {
    match m {
        ScramMechanism::Sha256 => "SCRAM-SHA-256",
        ScramMechanism::Sha1 => "SCRAM-SHA-1",
    }
}

fn parse_mechanism(name: &str) -> Option<ScramMechanism> {
    match name {
        "SCRAM-SHA-256" => Some(ScramMechanism::Sha256),
        "SCRAM-SHA-1" => Some(ScramMechanism::Sha1),
        _ => None,
    }
}

fn parse_scram_attrs(msg: &str) -> Option<Vec<(String, String)>> {
    let mut out = Vec::new();
    for part in msg.split(',') {
        let (k, v) = part.split_once('=')?;
        if k.is_empty() {
            return None;
        }
        out.push((k.to_string(), v.to_string()));
    }
    if out.is_empty() {
        return None;
    }
    Some(out)
}

fn scram_attr(attrs: &[(String, String)], key: &str) -> Option<String> {
    attrs
        .iter()
        .find(|(k, _)| k == key)
        .map(|(_, v)| v.clone())
}

/// SCRAM client side (RFC 5802 / RFC 7677). One instance = one
/// authentication attempt; the caller drives the saslStart /
/// saslContinue exchange. Pure RFC: the caller supplies the
/// effective password (MongoDB's SHA-1 MD5-hex transform is applied
/// by MongoClient::authenticate, not here).
struct ScramClient {
    mechanism: ScramMechanism,
    password: Vec<u8>,
    client_first_bare: String,
    full_nonce: String,
    salted_password: Vec<u8>,
    auth_message: String,
    have_server_first: bool,
}

impl ScramClient {
    /// nonce_b64: fixed client nonce (base64 text) for tests; empty
    /// generates a random nonce. Used verbatim as the SCRAM "r=" value.
    fn new(mechanism: ScramMechanism, username: String, password: Vec<u8>, nonce_b64: &str) -> Self {
        // SCRAM username escaping: '=' -> "=3D", ',' -> "=2C" (RFC 5802 §5.1).
        let mut escaped = String::new();
        for c in username.chars() {
            match c {
                '=' => escaped.push_str("=3D"),
                ',' => escaped.push_str("=2C"),
                _ => escaped.push(c),
            }
        }
        let nonce = if nonce_b64.is_empty() {
            random_nonce_base64(18)
        } else {
            nonce_b64.to_string()
        };
        let client_first_bare = format!("n={},r={}", escaped, nonce);
        ScramClient {
            mechanism,
            password,
            client_first_bare,
            full_nonce: String::new(),
            salted_password: Vec::new(),
            auth_message: String::new(),
            have_server_first: false,
        }
    }

    /// Full client-first-message: "n,,n=<user>,r=<nonce>".
    fn client_first(&self) -> String {
        format!("n,,{}", self.client_first_bare)
    }

    /// Parse the server-first-message ("r=<nonce>,s=<salt>,i=<iters>").
    /// Verifies the server nonce starts with our client nonce, then
    /// derives the salted password (PBKDF2).
    fn server_first(&mut self, server_first: &str) -> Result<(), String> {
        let attrs = parse_scram_attrs(server_first)
            .ok_or_else(|| format!("malformed SCRAM server-first-message: {}", server_first))?;
        let full_nonce = scram_attr(&attrs, "r").ok_or_else(|| {
            format!("malformed SCRAM server-first-message: {}", server_first)
        })?;
        let salt_b64 = scram_attr(&attrs, "s").ok_or_else(|| {
            format!("malformed SCRAM server-first-message: {}", server_first)
        })?;
        let iters_s = scram_attr(&attrs, "i").ok_or_else(|| {
            format!("malformed SCRAM server-first-message: {}", server_first)
        })?;
        // The server nonce must extend our client nonce (RFC 5802 §5.1).
        let client_nonce = self.client_first_bare
            .split_once("r=")
            .map(|(_, r)| r)
            .unwrap_or("");
        if !full_nonce.starts_with(client_nonce) {
            return Err("SCRAM server nonce does not start with client nonce".to_string());
        }
        let salt = base64_decode(&salt_b64)
            .ok_or_else(|| "SCRAM server-first-message has invalid base64 salt".to_string())?;
        let iterations: u32 = iters_s.parse().map_err(|_| {
            "SCRAM server-first-message has invalid iteration count".to_string()
        })?;
        if iterations < 1 {
            return Err("SCRAM server-first-message has invalid iteration count".to_string());
        }
        // Cap iterations so a hostile server cannot make us PBKDF2-loop
        // forever (MongoDB defaults to 10,000).
        if iterations > 1_000_000 {
            return Err("SCRAM iteration count unreasonably large".to_string());
        }
        let use256 = self.mechanism == ScramMechanism::Sha256;
        self.salted_password = if use256 {
            pbkdf2_sha256(&self.password, &salt, iterations, 32)
        } else {
            pbkdf2_sha1(&self.password, &salt, iterations, 20)
        };
        self.full_nonce = full_nonce;
        let client_final_wo_proof = format!("c=biws,r={}", self.full_nonce);
        self.auth_message = format!(
            "{},{},{}",
            self.client_first_bare, server_first, client_final_wo_proof
        );
        self.have_server_first = true;
        Ok(())
    }

    /// Client-final-message: "c=biws,r=<full nonce>,p=<base64 proof>".
    /// ("biws" is base64("n,,").) Requires server_first() to have run.
    fn client_final(&self) -> String {
        if !self.have_server_first {
            return String::new();
        }
        let use256 = self.mechanism == ScramMechanism::Sha256;
        let client_key = if use256 {
            hmac_sha256(&self.salted_password, b"Client Key")
        } else {
            hmac_sha1(&self.salted_password, b"Client Key")
        };
        let stored_key = if use256 {
            sha256(&client_key)
        } else {
            sha1(&client_key)
        };
        let client_sig = if use256 {
            hmac_sha256(&stored_key, self.auth_message.as_bytes())
        } else {
            hmac_sha1(&stored_key, self.auth_message.as_bytes())
        };
        let proof: Vec<u8> = client_key
            .iter()
            .zip(client_sig.iter())
            .map(|(a, b)| a ^ b)
            .collect();
        format!(
            "c=biws,r={},p={}",
            self.full_nonce,
            base64_encode(&proof)
        )
    }

    /// Verify the server-final-message ("v=<base64 ServerSignature>"
    /// on success, "e=<error>" on failure).
    fn server_final(&self, server_final: &str) -> Result<(), String> {
        if !self.have_server_first {
            return Err("SCRAM server-final received before server-first".to_string());
        }
        let attrs = parse_scram_attrs(server_final).ok_or_else(|| {
            format!("malformed SCRAM server-final-message: {}", server_final)
        })?;
        if let Some(e) = scram_attr(&attrs, "e") {
            return Err(format!("SCRAM authentication failed (server error): {}", e));
        }
        let verifier = scram_attr(&attrs, "v").ok_or_else(|| {
            "SCRAM server-final-message has no verifier (v=)".to_string()
        })?;
        let use256 = self.mechanism == ScramMechanism::Sha256;
        let server_key = if use256 {
            hmac_sha256(&self.salted_password, b"Server Key")
        } else {
            hmac_sha1(&self.salted_password, b"Server Key")
        };
        let expected = if use256 {
            base64_encode(&hmac_sha256(&server_key, self.auth_message.as_bytes()))
        } else {
            base64_encode(&hmac_sha1(&server_key, self.auth_message.as_bytes()))
        };
        if verifier != expected {
            return Err("SCRAM server signature mismatch (possible MITM)".to_string());
        }
        Ok(())
    }
}

// ------------------------------------------------------------------ BSON --
// Minimal BSON builder/parser for the value shapes the store uses:
// string, int32, int64, double, bool, document, array of docs, regex.
// Ported from core-cpp src/store.cpp (namespace bson).

fn put_i32(out: &mut Vec<u8>, v: i32) {
    out.extend_from_slice(&v.to_le_bytes());
}
fn put_i64(out: &mut Vec<u8>, v: i64) {
    out.extend_from_slice(&v.to_le_bytes());
}
fn put_cstr(out: &mut Vec<u8>, s: &str) {
    out.extend_from_slice(s.as_bytes());
    out.push(0);
}
fn wrap_doc(mut elements: Vec<u8>) -> Vec<u8> {
    let mut out = Vec::new();
    put_i32(&mut out, (elements.len() + 5) as i32);
    out.append(&mut elements);
    out.push(0);
    out
}
fn el_string(key: &str, value: &str) -> Vec<u8> {
    let mut out = vec![0x02];
    put_cstr(&mut out, key);
    put_i32(&mut out, (value.len() + 1) as i32);
    out.extend_from_slice(value.as_bytes());
    out.push(0);
    out
}
fn el_int32(key: &str, value: i32) -> Vec<u8> {
    let mut out = vec![0x10];
    put_cstr(&mut out, key);
    put_i32(&mut out, value);
    out
}
fn el_int64(key: &str, value: i64) -> Vec<u8> {
    let mut out = vec![0x12];
    put_cstr(&mut out, key);
    put_i64(&mut out, value);
    out
}
fn el_double(key: &str, value: f64) -> Vec<u8> {
    let mut out = vec![0x01];
    put_cstr(&mut out, key);
    out.extend_from_slice(&value.to_le_bytes());
    out
}
fn el_bool(key: &str, value: bool) -> Vec<u8> {
    let mut out = vec![0x08];
    put_cstr(&mut out, key);
    out.push(if value { 1 } else { 0 });
    out
}
fn el_doc(key: &str, doc_bson: &[u8]) -> Vec<u8> {
    let mut out = vec![0x03];
    put_cstr(&mut out, key);
    out.extend_from_slice(doc_bson);
    out
}
fn el_array_of_docs(key: &str, docs: &[Vec<u8>]) -> Vec<u8> {
    let mut elements = Vec::new();
    for (i, d) in docs.iter().enumerate() {
        elements.extend_from_slice(&el_doc(&i.to_string(), d));
    }
    let mut out = vec![0x04];
    put_cstr(&mut out, key);
    out.extend_from_slice(&wrap_doc(elements));
    out
}
fn el_regex(key: &str, pattern: &str) -> Vec<u8> {
    let mut out = vec![0x0b];
    put_cstr(&mut out, key);
    put_cstr(&mut out, pattern);
    put_cstr(&mut out, "");
    out
}
fn el_binary(key: &str, bytes: &[u8]) -> Vec<u8> {
    // BSON binary, subtype 0 (generic). Used for the SCRAM
    // saslStart / saslContinue payloads.
    let mut out = vec![0x05];
    put_cstr(&mut out, key);
    put_i32(&mut out, bytes.len() as i32);
    out.push(0);
    out.extend_from_slice(bytes);
    out
}
fn doc_of(mut fields: Vec<Vec<u8>>) -> Vec<u8> {
    let mut elements = Vec::new();
    for f in fields.drain(..) {
        elements.extend_from_slice(&f);
    }
    wrap_doc(elements)
}

fn read_i32(doc: &[u8], pos: usize) -> Option<i32> {
    if pos + 4 > doc.len() {
        return None;
    }
    Some(i32::from_le_bytes(doc[pos..pos + 4].try_into().ok()?))
}

/// Walk a BSON document; invoke f(type, key, value_offset) per element.
/// Returning false from f stops the walk (not an error). Returns false
/// when the document is malformed.
fn walk(doc: &[u8], f: &mut dyn FnMut(u8, &str, usize) -> bool) -> bool {
    if doc.len() < 5 {
        return false;
    }
    let mut pos = 4usize;
    while pos < doc.len() {
        let ty = doc[pos];
        if ty == 0 {
            return true;
        }
        let key_end = match doc[pos + 1..].iter().position(|&b| b == 0) {
            Some(i) => pos + 1 + i,
            None => return false,
        };
        let key = match std::str::from_utf8(&doc[pos + 1..key_end]) {
            Ok(k) => k,
            Err(_) => return false,
        };
        let vpos = key_end + 1;
        let next: usize = match ty {
            0x01 => vpos + 8,
            0x02 | 0x0d | 0x0e => {
                let len = match read_i32(doc, vpos) {
                    Some(l) if l >= 0 => l as usize,
                    _ => return false,
                };
                vpos + 4 + len
            }
            0x03 | 0x04 => {
                let len = match read_i32(doc, vpos) {
                    Some(l) if l >= 0 => l as usize,
                    _ => return false,
                };
                vpos + len
            }
            0x05 => {
                let len = match read_i32(doc, vpos) {
                    Some(l) if l >= 0 => l as usize,
                    _ => return false,
                };
                vpos + 5 + len
            }
            0x06 | 0x0a => vpos,
            0x07 => vpos + 12,
            0x08 => vpos + 1,
            0x09 | 0x11 | 0x12 => vpos + 8,
            0x0b => {
                let a = match doc[vpos..].iter().position(|&b| b == 0) {
                    Some(i) => vpos + i,
                    None => return false,
                };
                match doc[a + 1..].iter().position(|&b| b == 0) {
                    Some(i) => a + 1 + i + 1,
                    None => return false,
                }
            }
            0x0f => {
                let len = match read_i32(doc, vpos) {
                    Some(l) if l >= 0 => l as usize,
                    _ => return false,
                };
                vpos + len
            }
            0x10 => vpos + 4,
            0x13 => vpos + 16,
            _ => return false,
        };
        if next > doc.len() {
            return false;
        }
        if !f(ty, key, vpos) {
            return true;
        }
        pos = next;
    }
    false
}

fn read_string_at(doc: &[u8], vpos: usize) -> Option<String> {
    let len = read_i32(doc, vpos)?;
    if len <= 0 || vpos + 4 + (len as usize) > doc.len() {
        return None;
    }
    let bytes = &doc[vpos + 4..vpos + 4 + (len as usize) - 1];
    String::from_utf8(bytes.to_vec()).ok()
}

fn get_string(doc: &[u8], key: &str) -> Option<String> {
    let mut out = None;
    walk(doc, &mut |ty, k, vpos| {
        if k == key && ty == 0x02 {
            out = read_string_at(doc, vpos);
            return false;
        }
        true
    });
    out
}

fn get_int32(doc: &[u8], key: &str) -> Option<i32> {
    let mut out = None;
    walk(doc, &mut |ty, k, vpos| {
        if k == key && ty == 0x10 {
            out = read_i32(doc, vpos);
            return false;
        }
        true
    });
    out
}

/// Raw bytes of the BSON binary field `key` (type 0x05), if present.
fn get_binary(doc: &[u8], key: &str) -> Option<Vec<u8>> {
    let mut out = None;
    walk(doc, &mut |ty, k, vpos| {
        if k == key && ty == 0x05 {
            if let Some(len) = read_i32(doc, vpos) {
                if len >= 0 && vpos + 5 + (len as usize) <= doc.len() {
                    out = Some(doc[vpos + 5..vpos + 5 + (len as usize)].to_vec());
                }
            }
            return false;
        }
        true
    });
    out
}

fn get_double(doc: &[u8], key: &str) -> Option<f64> {
    let mut out = None;
    walk(doc, &mut |ty, k, vpos| {
        if k != key {
            return true;
        }
        match ty {
            0x01 => {
                if vpos + 8 <= doc.len() {
                    out = Some(f64::from_le_bytes(
                        doc[vpos..vpos + 8].try_into().ok().unwrap_or([0; 8]),
                    ));
                }
                false
            }
            0x10 => {
                out = read_i32(doc, vpos).map(|v| v as f64);
                false
            }
            0x12 => {
                if vpos + 8 <= doc.len() {
                    out = Some(i64::from_le_bytes(
                        doc[vpos..vpos + 8].try_into().ok().unwrap_or([0; 8]),
                    ) as f64);
                }
                false
            }
            _ => true,
        }
    });
    out
}

/// Raw bytes of the subdocument at `key` (type 0x03), if present.
fn get_doc(doc: &[u8], key: &str) -> Option<Vec<u8>> {
    let mut out = None;
    walk(doc, &mut |ty, k, vpos| {
        if k == key && ty == 0x03 {
            if let Some(len) = read_i32(doc, vpos) {
                if len > 0 && vpos + (len as usize) <= doc.len() {
                    out = Some(doc[vpos..vpos + (len as usize)].to_vec());
                }
            }
            return false;
        }
        true
    });
    out
}

/// First document of the array field `key`, raw BSON bytes.
fn get_first_array_doc(doc: &[u8], key: &str) -> Option<Vec<u8>> {
    let mut out = None;
    walk(doc, &mut |ty, k, vpos| {
        if k != key || ty != 0x04 {
            return true;
        }
        let len = match read_i32(doc, vpos) {
            Some(l) if l > 0 => l as usize,
            _ => return false,
        };
        if vpos + len > doc.len() {
            return false;
        }
        let arr = &doc[vpos..vpos + len];
        walk(arr, &mut |t2, k2, v2| {
            if k2 == "0" && t2 == 0x03 {
                if let Some(dlen) = read_i32(arr, v2) {
                    if dlen > 0 && v2 + (dlen as usize) <= arr.len() {
                        out = Some(arr[v2..v2 + (dlen as usize)].to_vec());
                    }
                }
                return false;
            }
            true
        });
        false
    });
    out
}

/// Every document of the array field `key`, raw BSON bytes each.
fn get_array_docs(doc: &[u8], key: &str) -> Vec<Vec<u8>> {
    let mut out = Vec::new();
    walk(doc, &mut |ty, k, vpos| {
        if k != key || ty != 0x04 {
            return true;
        }
        let len = match read_i32(doc, vpos) {
            Some(l) if l > 0 => l as usize,
            _ => return false,
        };
        if vpos + len > doc.len() {
            return false;
        }
        let arr = &doc[vpos..vpos + len];
        walk(arr, &mut |t2, _k2, v2| {
            if t2 == 0x03 {
                if let Some(dlen) = read_i32(arr, v2) {
                    if dlen > 0 && v2 + (dlen as usize) <= arr.len() {
                        out.push(arr[v2..v2 + (dlen as usize)].to_vec());
                    }
                }
            }
            true
        });
        false
    });
    out
}

// ------------------------------------------------------------- TCP helper --

fn connect_tcp(host: &str, port: u16) -> Result<TcpStream, String> {
    let addr = format!("{}:{}", host, port);
    let mut last_err = String::new();
    for sock in addr.to_socket_addrs().map_err(|e| e.to_string())? {
        match TcpStream::connect_timeout(&sock, IO_TIMEOUT) {
            Ok(s) => {
                let _ = s.set_read_timeout(Some(IO_TIMEOUT));
                let _ = s.set_write_timeout(Some(IO_TIMEOUT));
                return Ok(s);
            }
            Err(e) => last_err = e.to_string(),
        }
    }
    Err(last_err)
}

// ----------------------------------------------------------------- Redis --
// Minimal RESP client (GET/SET/DEL/KEYS/PING + AUTH/SELECT), ported from
// core-cpp RedisClient. One connection, sequential use.

pub struct RedisClient {
    stream: TcpStream,
    read_buf: Vec<u8>,
}

enum Reply {
    Simple(String),
    Error(String),
    Int(i64),
    Bulk(Option<String>),
    Array(Vec<Reply>),
}

impl RedisClient {
    /// url: redis://[:pass@]host[:port][/db]
    pub fn connect(url: &str) -> Result<RedisClient, String> {
        let mut rest = url
            .strip_prefix("redis://")
            .ok_or_else(|| "redis URL must start with redis://".to_string())?
            .to_string();
        let mut password = String::new();
        if let Some(at) = rest.find('@') {
            let userinfo = rest[..at].to_string();
            rest = rest[at + 1..].to_string();
            password = match userinfo.find(':') {
                Some(c) => userinfo[c + 1..].to_string(),
                None => userinfo,
            };
        }
        let mut host = "127.0.0.1".to_string();
        let mut port: u16 = 6379;
        let mut db: i64 = 0;
        if let Some(slash) = rest.find('/') {
            db = rest[slash + 1..].parse().unwrap_or(0);
            rest = rest[..slash].to_string();
        }
        if let Some(colon) = rest.rfind(':') {
            host = rest[..colon].to_string();
            port = rest[colon + 1..].parse().map_err(|_| "bad redis port".to_string())?;
        } else if !rest.is_empty() {
            host = rest;
        }
        let stream = connect_tcp(&host, port)
            .map_err(|e| format!("cannot connect to redis at {}:{}: {}", host, port, e))?;
        let mut client = RedisClient {
            stream,
            read_buf: Vec::new(),
        };
        if !password.is_empty() {
            let r = client.command(&["AUTH", &password])?;
            if let Reply::Error(e) = r {
                return Err(format!("redis AUTH failed: {}", e));
            }
        }
        if db != 0 {
            let r = client.command(&["SELECT", &db.to_string()])?;
            if let Reply::Error(e) = r {
                return Err(format!("redis SELECT failed: {}", e));
            }
        }
        client.ping()?;
        Ok(client)
    }

    pub fn ping(&mut self) -> Result<(), String> {
        match self.command(&["PING"])? {
            Reply::Simple(s) if s == "PONG" => Ok(()),
            Reply::Error(e) => Err(format!("redis PING failed: {}", e)),
            _ => Err("redis PING unexpected reply".to_string()),
        }
    }

    fn read_line(&mut self) -> Result<String, String> {
        loop {
            if let Some(pos) = self.read_buf.iter().position(|&b| b == b'\n') {
                let line: Vec<u8> = self.read_buf.drain(..=pos).collect();
                let mut line = line;
                if line.last() == Some(&b'\n') {
                    line.pop();
                }
                if line.last() == Some(&b'\r') {
                    line.pop();
                }
                return String::from_utf8(line)
                    .map_err(|_| "redis reply not UTF-8".to_string());
            }
            let mut tmp = [0u8; 4096];
            let n = self
                .stream
                .read(&mut tmp)
                .map_err(|e| format!("redis read failed: {}", e))?;
            if n == 0 {
                return Err("redis connection closed".to_string());
            }
            self.read_buf.extend_from_slice(&tmp[..n]);
        }
    }

    fn read_exact(&mut self, n: usize) -> Result<Vec<u8>, String> {
        let mut out = Vec::new();
        if !self.read_buf.is_empty() {
            let take = n.min(self.read_buf.len());
            out.extend_from_slice(&self.read_buf.drain(..take).collect::<Vec<u8>>());
        }
        while out.len() < n {
            let mut tmp = vec![0u8; n - out.len()];
            self.stream
                .read_exact(&mut tmp)
                .map_err(|e| format!("redis read failed: {}", e))?;
            out.extend_from_slice(&tmp);
        }
        Ok(out)
    }

    fn command(&mut self, argv: &[&str]) -> Result<Reply, String> {
        let mut wire = format!("*{}\r\n", argv.len());
        for a in argv {
            wire.push_str(&format!("${}\r\n{}\r\n", a.len(), a));
        }
        self.stream
            .write_all(wire.as_bytes())
            .map_err(|e| format!("redis write failed: {}", e))?;
        self.read_reply()
    }

    fn read_reply(&mut self) -> Result<Reply, String> {
        let line = self.read_line()?;
        if line.is_empty() {
            return Err("redis empty reply".to_string());
        }
        let (kind, payload) = (line.as_bytes()[0], &line[1..]);
        match kind {
            b'+' => Ok(Reply::Simple(payload.to_string())),
            b'-' => Ok(Reply::Error(payload.to_string())),
            b':' => Ok(Reply::Int(payload.parse().unwrap_or(0))),
            b'$' => {
                let len: i64 = payload.parse().map_err(|_| "bad bulk length".to_string())?;
                if len < 0 {
                    return Ok(Reply::Bulk(None));
                }
                let buf = self.read_exact(len as usize)?;
                let _trailer = self.read_exact(2)?;
                Ok(Reply::Bulk(Some(
                    String::from_utf8(buf).map_err(|_| "redis bulk not UTF-8".to_string())?,
                )))
            }
            b'*' => {
                let count: i64 = payload.parse().map_err(|_| "bad array length".to_string())?;
                if count < 0 {
                    return Ok(Reply::Array(vec![]));
                }
                let mut items = Vec::new();
                for _ in 0..count {
                    items.push(self.read_reply()?);
                }
                Ok(Reply::Array(items))
            }
            _ => Err(format!("unsupported RESP type: {}", kind as char)),
        }
    }

    pub fn get(&mut self, key: &str) -> Result<Option<String>, String> {
        match self.command(&["GET", key])? {
            Reply::Bulk(v) => Ok(v),
            Reply::Error(e) => Err(format!("redis GET failed: {}", e)),
            _ => Err("redis GET unexpected reply".to_string()),
        }
    }

    pub fn set(&mut self, key: &str, value: &str) -> Result<(), String> {
        match self.command(&["SET", key, value])? {
            Reply::Error(e) => Err(format!("redis SET failed: {}", e)),
            _ => Ok(()),
        }
    }

    pub fn del(&mut self, key: &str) -> Result<(), String> {
        match self.command(&["DEL", key])? {
            Reply::Error(e) => Err(format!("redis DEL failed: {}", e)),
            _ => Ok(()),
        }
    }

    pub fn keys(&mut self, pattern: &str) -> Result<Vec<String>, String> {
        match self.command(&["KEYS", pattern])? {
            Reply::Array(items) => {
                let mut out = Vec::new();
                for it in items {
                    if let Reply::Bulk(Some(s)) = it {
                        out.push(s);
                    }
                }
                Ok(out)
            }
            Reply::Error(e) => Err(format!("redis KEYS failed: {}", e)),
            _ => Err("redis KEYS unexpected reply".to_string()),
        }
    }
}

// ----------------------------------------------------------------- Mongo --
// Minimal MongoDB wire-protocol client (OP_MSG + BSON subset), ported
// from core-cpp MongoClient, including SCRAM-SHA-256 / SCRAM-SHA-1
// authentication (saslStart / saslContinue against the authSource
// database). Same negotiation as core-cpp (A7): the URI's
// ?authMechanism= when set, else SCRAM-SHA-256 first with a fall-back
// to SCRAM-SHA-1 when the server reports the mechanism unusable for
// the user. A wrong password fails at saslContinue with the server's
// AuthenticationFailed error — never a silent success.

pub struct MongoClient {
    stream: TcpStream,
    host: String,
    port: u16,
    db: String,
    username: String,
    password: String,
    auth_source: String,
    auth_mechanism: Option<String>,
    request_id: i32,
}

struct MongoUri {
    host: String,
    port: u16,
    db: String,
    username: String,
    password: String,
    auth_source: String,
    auth_mechanism: Option<String>,
}

fn parse_mongo_uri(uri: &str) -> Result<MongoUri, String> {
    let rest = uri.strip_prefix("mongodb://").ok_or_else(|| {
        if uri.starts_with("mongodb+srv://") {
            "mongodb+srv:// URIs are not supported by the minimal wire-protocol client (no DNS SRV lookup); use mongodb:// with an explicit host:port".to_string()
        } else {
            "mongodb URI must start with mongodb://".to_string()
        }
    })?;
    let mut rest = rest.to_string();
    // Query string first: authSource, authMechanism (others ignored).
    let mut query = String::new();
    if let Some(q) = rest.find('?') {
        query = rest[q + 1..].to_string();
        rest = rest[..q].to_string();
    }
    let mut auth_source = String::new();
    let mut auth_mechanism: Option<String> = None;
    for pair in query.split('&') {
        if pair.is_empty() {
            continue;
        }
        let (key, value) = match pair.split_once('=') {
            Some((k, v)) => (k, v),
            None => (pair, ""),
        };
        let decoded = pct_decode(value)
            .ok_or_else(|| "bad percent-escape in mongodb URI query".to_string())?;
        if key == "authSource" {
            auth_source = decoded;
        } else if key == "authMechanism" {
            if parse_mechanism(&decoded).is_none() {
                return Err(format!(
                    "unsupported authMechanism in mongodb URI: {} (only SCRAM-SHA-1 / SCRAM-SHA-256)",
                    decoded
                ));
            }
            auth_mechanism = Some(decoded);
        }
    }
    // Path = database (may be empty -> default "parlay").
    let mut db = "parlay".to_string();
    let mut dbpart_empty = true;
    if let Some(slash) = rest.find('/') {
        let dbpart = rest[slash + 1..].to_string();
        rest = rest[..slash].to_string();
        if !dbpart.is_empty() {
            dbpart_empty = false;
            db = pct_decode(&dbpart)
                .ok_or_else(|| "bad percent-escape in mongodb database name".to_string())?;
        }
    }
    // authSource default: the database in the URI, else "admin".
    if auth_source.is_empty() {
        auth_source = if !dbpart_empty { db.clone() } else { "admin".to_string() };
    }
    // Authority: [userinfo@]host[:port]. Userinfo ends at the LAST
    // '@' (a password may contain a percent-encoded '@', never a raw
    // one). Credentials are SCRAM-authenticated (see authenticate()).
    let mut username = String::new();
    let mut password = String::new();
    if let Some(at) = rest.rfind('@') {
        let userinfo = rest[..at].to_string();
        rest = rest[at + 1..].to_string();
        if !userinfo.is_empty() {
            let (user, pass) = match userinfo.find(':') {
                Some(c) => (&userinfo[..c], &userinfo[c + 1..]),
                None => (userinfo.as_str(), ""),
            };
            username = pct_decode(user)
                .ok_or_else(|| "bad percent-escape in mongodb credentials".to_string())?;
            password = pct_decode(pass)
                .ok_or_else(|| "bad percent-escape in mongodb credentials".to_string())?;
            if username.is_empty() {
                return Err("mongodb URI has an empty username".to_string());
            }
        }
    }
    if rest.is_empty() {
        return Err("mongodb URI has no host".to_string());
    }
    if rest.contains(',') {
        return Err(
            "mongodb URIs with multiple hosts (replica sets) are not supported by the minimal wire-protocol client"
                .to_string(),
        );
    }
    let (host, port) = if let Some(stripped) = rest.strip_prefix('[') {
        let close = stripped
            .find(']')
            .ok_or_else(|| "malformed IPv6 host in mongodb URI".to_string())?;
        let host = stripped[..close].to_string();
        let mut port: u16 = 27017;
        let tail = &stripped[close + 1..];
        if !tail.is_empty() {
            if !tail.starts_with(':') {
                return Err("malformed host:port in mongodb URI".to_string());
            }
            port = tail[1..]
                .parse()
                .map_err(|_| "bad port in mongodb URI".to_string())?;
        }
        (host, port)
    } else if let Some(colon) = rest.rfind(':') {
        let host = rest[..colon].to_string();
        let port: u16 = rest[colon + 1..]
            .parse()
            .map_err(|_| "bad port in mongodb URI".to_string())?;
        (host, port)
    } else {
        (rest.clone(), 27017)
    };
    if host.is_empty() || port == 0 {
        return Err("bad host:port in mongodb URI".to_string());
    }
    Ok(MongoUri {
        host,
        port,
        db,
        username,
        password,
        auth_source,
        auth_mechanism,
    })
}

impl MongoClient {
    pub fn connect(uri: &str) -> Result<MongoClient, String> {
        let parsed = parse_mongo_uri(uri)?;
        let stream = connect_tcp(&parsed.host, parsed.port).map_err(|e| {
            format!(
                "cannot connect to mongodb at {}:{}: {}",
                parsed.host, parsed.port, e
            )
        })?;
        let mut client = MongoClient {
            stream,
            host: parsed.host,
            port: parsed.port,
            db: parsed.db,
            username: parsed.username,
            password: parsed.password,
            auth_source: parsed.auth_source,
            auth_mechanism: parsed.auth_mechanism,
            request_id: 1,
        };
        if !client.username.is_empty() {
            client.authenticate()?;
        }
        client.ping()?;
        Ok(client)
    }

    /// SCRAM handshake (RFC 5802 / RFC 7677) via saslStart /
    /// saslContinue. Mechanism order: the URI's ?authMechanism= when
    /// set, else SCRAM-SHA-256 first with a fall-back to SCRAM-SHA-1
    /// if the server reports the mechanism unsupported (e.g. a user
    /// created with mechanisms ["SCRAM-SHA-1"] only). A wrong
    /// password fails here with the server's AuthenticationFailed
    /// error — never a silent success.
    fn authenticate(&mut self) -> Result<(), String> {
        let mechanisms: Vec<ScramMechanism> = match &self.auth_mechanism {
            Some(m) => vec![parse_mechanism(m).ok_or_else(|| {
                format!("unsupported authMechanism in mongodb URI: {}", m)
            })?],
            None => vec![ScramMechanism::Sha256, ScramMechanism::Sha1],
        };
        let mut last_error = String::new();
        let n_mech = mechanisms.len();
        for (idx, mech) in mechanisms.iter().enumerate() {
            // MongoDB quirk: SCRAM-SHA-1 credentials are derived from
            // hex(MD5(username + ":mongo:" + password)), not the raw
            // password (see mongo_sha1_password). SCRAM-SHA-256 uses
            // the raw password.
            let eff_password = if *mech == ScramMechanism::Sha1 {
                mongo_sha1_password(&self.username, &self.password)
            } else {
                self.password.clone()
            };
            let mut sc = ScramClient::new(
                *mech,
                self.username.clone(),
                eff_password.into_bytes(),
                "",
            );
            // saslStart: payload is BinData(0, <raw client-first bytes>).
            let start = doc_of(vec![
                el_int32("saslStart", 1),
                el_string("mechanism", mechanism_name(*mech)),
                el_binary("payload", sc.client_first().as_bytes()),
                el_bool("autoAuthorize", true),
                el_doc(
                    "options",
                    &doc_of(vec![el_bool("skipEmptyExchange", true)]),
                ),
                el_string("$db", &self.auth_source),
            ]);
            let reply = match self.run_command_raw(&start) {
                Ok(r) => r,
                Err(e) => {
                    return Err(format!("saslStart transport failed: {}", e));
                }
            };
            let ok = get_double(&reply, "ok").unwrap_or(0.0);
            if ok != 1.0 {
                let msg = get_string(&reply, "errmsg").unwrap_or_default();
                last_error = format!(
                    "saslStart ({}) failed: {}",
                    mechanism_name(*mech),
                    msg
                );
                // Fall through to the next mechanism when negotiating:
                // any saslStart failure means this mechanism is
                // unusable for this user on this server. A genuinely
                // wrong password fails at saslContinue, not here, so
                // this cannot mask a bad password.
                if self.auth_mechanism.is_none() && idx + 1 < n_mech {
                    continue;
                }
                return Err(last_error);
            }
            let conversation_id = match get_int32(&reply, "conversationId") {
                Some(id) => id,
                None => {
                    return Err(
                        "saslStart reply missing conversationId/payload".to_string()
                    );
                }
            };
            let server_first_bytes = match get_binary(&reply, "payload") {
                Some(b) => b,
                None => {
                    return Err(
                        "saslStart reply missing conversationId/payload".to_string()
                    );
                }
            };
            let server_first = String::from_utf8(server_first_bytes).map_err(|_| {
                "saslStart reply payload is not UTF-8".to_string()
            })?;
            if let Err(e) = sc.server_first(&server_first) {
                return Err(format!("SCRAM server-first rejected: {}", e));
            }
            let cont = doc_of(vec![
                el_int32("saslContinue", 1),
                el_int32("conversationId", conversation_id),
                el_binary("payload", sc.client_final().as_bytes()),
                el_string("$db", &self.auth_source),
            ]);
            let reply = match self.run_command_raw(&cont) {
                Ok(r) => r,
                Err(e) => {
                    return Err(format!("saslContinue transport failed: {}", e));
                }
            };
            let ok = get_double(&reply, "ok").unwrap_or(0.0);
            if ok != 1.0 {
                let msg = get_string(&reply, "errmsg").unwrap_or_default();
                // Wrong password lands here (code 18, AuthenticationFailed).
                return Err(format!(
                    "SCRAM authentication failed for user '{}' ({}): {}",
                    self.username,
                    mechanism_name(*mech),
                    msg
                ));
            }
            let server_final_bytes = match get_binary(&reply, "payload") {
                Some(b) => b,
                None => {
                    return Err("saslContinue reply missing payload".to_string());
                }
            };
            let server_final =
                String::from_utf8(server_final_bytes)
                    .map_err(|_| "saslContinue reply payload is not UTF-8".to_string())?;
            if let Err(e) = sc.server_final(&server_final) {
                return Err(format!("SCRAM server-final rejected: {}", e));
            }
            return Ok(());
        }
        Err(last_error)
    }

    fn run_command_raw(&mut self, body_bson: &[u8]) -> Result<Vec<u8>, String> {
        // OP_MSG: header(16) + flagBits(4) + section kind(1) + body doc.
        let total = 16 + 4 + 1 + body_bson.len() as i32;
        let mut msg = Vec::new();
        put_i32(&mut msg, total);
        put_i32(&mut msg, self.request_id);
        self.request_id = self.request_id.wrapping_add(1);
        put_i32(&mut msg, 0);
        put_i32(&mut msg, 2013); // OP_MSG
        put_i32(&mut msg, 0); // flagBits
        msg.push(0); // section kind 0: body
        msg.extend_from_slice(body_bson);
        self.stream
            .write_all(&msg)
            .map_err(|e| format!("mongodb send failed: {}", e))?;
        let mut header = [0u8; 16];
        self.stream
            .read_exact(&mut header)
            .map_err(|e| format!("mongodb recv failed: {}", e))?;
        let len = i32::from_le_bytes(header[0..4].try_into().unwrap());
        let op = i32::from_le_bytes(header[12..16].try_into().unwrap());
        if len < 16 || op != 2013 {
            return Err("mongodb bad reply header".to_string());
        }
        let mut rest = vec![0u8; (len as usize) - 16];
        self.stream
            .read_exact(&mut rest)
            .map_err(|e| format!("mongodb recv body failed: {}", e))?;
        if rest.len() < 5 || rest[4] != 0 {
            return Err("mongodb unexpected reply sections".to_string());
        }
        let body = &rest[5..];
        if body.len() < 4 {
            return Err("mongodb short body".to_string());
        }
        let blen = i32::from_le_bytes(body[0..4].try_into().unwrap());
        if blen > 0 && (blen as usize) <= body.len() {
            return Ok(body[..blen as usize].to_vec());
        }
        Ok(body.to_vec())
    }

    fn run_command(&mut self, body_bson: &[u8]) -> Result<Vec<u8>, String> {
        let body = self.run_command_raw(body_bson)?;
        match get_double(&body, "ok") {
            Some(1.0) => Ok(body),
            _ => {
                let msg = get_string(&body, "errmsg").unwrap_or_default();
                Err(format!("mongodb command failed: {}", msg))
            }
        }
    }

    pub fn ping(&mut self) -> Result<(), String> {
        let body = doc_of(vec![
            el_int32("ping", 1),
            el_string("$db", "admin"),
        ]);
        self.run_command(&body).map(|_| ())
    }

    /// Find one document's `v` field by _id. Ok(None) when absent.
    pub fn find_value(&mut self, key: &str) -> Result<Option<String>, String> {
        let filter = doc_of(vec![el_string("_id", key)]);
        let body = doc_of(vec![
            el_string("find", MONGO_COLLECTION),
            el_doc("filter", &filter),
            el_int64("limit", 1),
            el_string("$db", &self.db),
        ]);
        let reply = self.run_command(&body)?;
        let cur = match get_doc(&reply, "cursor") {
            Some(c) => c,
            None => return Ok(None),
        };
        match get_first_array_doc(&cur, "firstBatch") {
            Some(first) => Ok(get_string(&first, "v")),
            None => Ok(None),
        }
    }

    pub fn upsert_value(&mut self, key: &str, value: &str) -> Result<(), String> {
        let q = doc_of(vec![el_string("_id", key)]);
        let set = doc_of(vec![el_string("v", value)]);
        let u = doc_of(vec![el_doc("$set", &set)]);
        let upd = doc_of(vec![
            el_doc("q", &q),
            el_doc("u", &u),
            el_bool("upsert", true),
        ]);
        let body = doc_of(vec![
            el_string("update", MONGO_COLLECTION),
            el_array_of_docs("updates", &[upd]),
            el_string("$db", &self.db),
        ]);
        self.run_command(&body).map(|_| ())
    }

    pub fn delete_key(&mut self, key: &str) -> Result<(), String> {
        let q = doc_of(vec![el_string("_id", key)]);
        let del = doc_of(vec![el_doc("q", &q), el_int32("limit", 1)]);
        let body = doc_of(vec![
            el_string("delete", MONGO_COLLECTION),
            el_array_of_docs("deletes", &[del]),
            el_string("$db", &self.db),
        ]);
        self.run_command(&body).map(|_| ())
    }

    /// All _ids starting with `prefix` (full keys, `kv:` prefix NOT
    /// included — callers use the same key space as get/set/del).
    pub fn keys_with_prefix(&mut self, prefix: &str) -> Result<Vec<String>, String> {
        let filter = doc_of(vec![el_regex("_id", &format!("^{}", prefix))]);
        let body = doc_of(vec![
            el_string("find", MONGO_COLLECTION),
            el_doc("filter", &filter),
            el_string("$db", &self.db),
        ]);
        let reply = self.run_command(&body)?;
        let cur = match get_doc(&reply, "cursor") {
            Some(c) => c,
            None => return Ok(vec![]),
        };
        let docs = get_array_docs(&cur, "firstBatch");
        let mut out = Vec::new();
        for d in docs {
            if let Some(id) = get_string(&d, "_id") {
                out.push(id);
            }
        }
        Ok(out)
    }

    pub fn db_name(&self) -> &str {
        &self.db
    }
}

// ------------------------------------------------------------ SharedStore --
// The process-wide handle. `init()` must be called once at startup
// (before the first load); services exit(1) when it errors.

pub enum SharedStore {
    Mongo(MongoClient),
    Redis(RedisClient),
}

static STORE: Mutex<Option<SharedStore>> = Mutex::new(None);

fn mongo_key(key: &str) -> String {
    format!("kv:{}", key)
}
fn redis_key(key: &str) -> String {
    format!("parlay:kv:{}", key)
}

/// Connect the configured shared backend, or Ok(()) with backend "file"
/// when neither MONGO_URI nor REDIS_URL is set. Err when a configured
/// backend cannot be reached (fatal — see module docs).
pub fn init() -> Result<(), String> {
    let mongo_uri = std::env::var("MONGO_URI").unwrap_or_default();
    let redis_url = std::env::var("REDIS_URL").unwrap_or_default();
    init_from(&mongo_uri, &redis_url)
}

/// Same as init(), but with explicit URIs (used by service-level
/// tests, which pass PARLAY_TEST_* values instead of process env).
pub fn init_from(mongo_uri: &str, redis_url: &str) -> Result<(), String> {
    let mut guard = STORE.lock().map_err(|e| format!("store lock: {}", e))?;
    if guard.is_some() {
        return Ok(());
    }
    if !mongo_uri.is_empty() {
        let client = MongoClient::connect(mongo_uri)?;
        eprintln!(
            "[store] backend=mongo mongo={}:{} db={}",
            client.host,
            client.port,
            client.db_name()
        );
        *guard = Some(SharedStore::Mongo(client));
        return Ok(());
    }
    if !redis_url.is_empty() {
        let client = RedisClient::connect(redis_url)?;
        eprintln!("[store] backend=redis redis={}", strip_credentials(redis_url));
        *guard = Some(SharedStore::Redis(client));
        return Ok(());
    }
    eprintln!("[store] backend=file (MONGO_URI and REDIS_URL unset)");
    Ok(())
}

/// "mongo" | "redis" | "file".
pub fn backend() -> &'static str {
    match STORE.lock() {
        Ok(g) => match *g {
            Some(SharedStore::Mongo(_)) => "mongo",
            Some(SharedStore::Redis(_)) => "redis",
            None => "file",
        },
        Err(_) => "file",
    }
}

pub fn describe() -> String {
    match STORE.lock() {
        Ok(g) => match &*g {
            Some(SharedStore::Mongo(c)) => {
                format!("mongo {}:{}/{} state", c.host, c.port, c.db)
            }
            Some(SharedStore::Redis(_)) => "redis".to_string(),
            None => "file".to_string(),
        },
        Err(_) => "file".to_string(),
    }
}

pub fn get(key: &str) -> Result<Option<String>, String> {
    let mut guard = STORE.lock().map_err(|e| format!("store lock: {}", e))?;
    match guard.as_mut() {
        Some(SharedStore::Mongo(c)) => c.find_value(&mongo_key(key)),
        Some(SharedStore::Redis(c)) => c.get(&redis_key(key)),
        None => Err("shared store not configured (file backend)".to_string()),
    }
}

pub fn set(key: &str, value: &str) -> Result<(), String> {
    let mut guard = STORE.lock().map_err(|e| format!("store lock: {}", e))?;
    match guard.as_mut() {
        Some(SharedStore::Mongo(c)) => c.upsert_value(&mongo_key(key), value),
        Some(SharedStore::Redis(c)) => c.set(&redis_key(key), value),
        None => Err("shared store not configured (file backend)".to_string()),
    }
}

pub fn del(key: &str) -> Result<(), String> {
    let mut guard = STORE.lock().map_err(|e| format!("store lock: {}", e))?;
    match guard.as_mut() {
        Some(SharedStore::Mongo(c)) => c.delete_key(&mongo_key(key)),
        Some(SharedStore::Redis(c)) => c.del(&redis_key(key)),
        None => Err("shared store not configured (file backend)".to_string()),
    }
}

/// Keys starting with `prefix`, without any backend prefix.
pub fn keys(prefix: &str) -> Result<Vec<String>, String> {
    let mut guard = STORE.lock().map_err(|e| format!("store lock: {}", e))?;
    match guard.as_mut() {
        Some(SharedStore::Mongo(c)) => {
            let full = c.keys_with_prefix(&mongo_key(prefix))?;
            Ok(full
                .iter()
                .filter_map(|k| k.strip_prefix("kv:").map(|s| s.to_string()))
                .collect())
        }
        Some(SharedStore::Redis(c)) => {
            let full = c.keys(&format!("{}*", redis_key(prefix)))?;
            Ok(full
                .iter()
                .filter_map(|k| k.strip_prefix("parlay:kv:").map(|s| s.to_string()))
                .collect())
        }
        None => Err("shared store not configured (file backend)".to_string()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bson_round_trip() {
        let d = doc_of(vec![
            el_string("s", "hello"),
            el_int32("i32", -42),
            el_int64("i64", 1_700_000_000_123),
            el_double("dbl", 2.5),
            el_bool("b", true),
            el_doc("sub", &doc_of(vec![el_string("x", "y")])),
            el_array_of_docs(
                "arr",
                &[doc_of(vec![el_string("v", "one")]), doc_of(vec![el_string("v", "two")])],
            ),
            el_regex("rx", "^kv:"),
        ]);
        assert_eq!(get_string(&d, "s").as_deref(), Some("hello"));
        assert_eq!(get_double(&d, "i32"), Some(-42.0));
        assert_eq!(get_double(&d, "i64"), Some(1_700_000_000_123.0));
        assert_eq!(get_double(&d, "dbl"), Some(2.5));
        let sub = get_doc(&d, "sub").expect("sub doc");
        assert_eq!(get_string(&sub, "x").as_deref(), Some("y"));
        let first = get_first_array_doc(&d, "arr").expect("first array doc");
        assert_eq!(get_string(&first, "v").as_deref(), Some("one"));
        let docs = get_array_docs(&d, "arr");
        assert_eq!(docs.len(), 2);
        assert_eq!(get_string(&docs[1], "v").as_deref(), Some("two"));
        // Walk visits every key once.
        let mut seen = Vec::new();
        assert!(walk(&d, &mut |_, k, _| {
            seen.push(k.to_string());
            true
        }));
        assert_eq!(
            seen,
            vec!["s", "i32", "i64", "dbl", "b", "sub", "arr", "rx"]
        );
    }

    #[test]
    fn mongo_uri_parse() {
        let u = parse_mongo_uri("mongodb://127.0.0.1:27099/parlay").unwrap();
        assert_eq!((u.host, u.port, u.db), ("127.0.0.1".into(), 27099, "parlay".into()));
        assert_eq!(u.username, "");
        assert_eq!(u.auth_source, "parlay");
        assert_eq!(u.auth_mechanism, None);
        let u = parse_mongo_uri("mongodb://mongo:27017").unwrap();
        assert_eq!(u.db, "parlay");
        assert_eq!(u.auth_source, "admin");
        // Credentials: parsed (pct-decoded), no longer rejected.
        let u = parse_mongo_uri("mongodb://user:p%40ss%3Aw%2Frd@h:27017/db").unwrap();
        assert_eq!(u.username, "user");
        assert_eq!(u.password, "p@ss:w/rd");
        assert_eq!(u.auth_source, "db");
        let u = parse_mongo_uri(
            "mongodb://scram256:dummy-pass-256@127.0.0.1:27098/parlay_test?authSource=admin",
        )
        .unwrap();
        assert_eq!(u.auth_source, "admin");
        assert_eq!(u.auth_mechanism, None);
        let u = parse_mongo_uri(
            "mongodb://scram1:dummy-pass-1@127.0.0.1:27098/parlay_test?authMechanism=SCRAM-SHA-1",
        )
        .unwrap();
        assert_eq!(u.auth_mechanism, Some("SCRAM-SHA-1".to_string()));
        assert!(parse_mongo_uri("mongodb://h:27017/db?authMechanism=PLAIN").is_err());
        assert!(parse_mongo_uri("mongodb+srv://x/y").is_err());
        assert!(parse_mongo_uri("mongodb://a:27017,b:27017/db").is_err());
        assert!(parse_mongo_uri("mongodb://:pass@h:27017/db").is_err());
        assert!(parse_mongo_uri("http://h:27017/db").is_err());
    }

    #[test]
    fn scram_crypto_vectors() {
        // Vectors verified against Python hashlib/hmac (RFC 6070
        // PBKDF2) — same set as core-cpp scram_test.cpp.
        assert_eq!(hex_encode(&sha1(b"abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
        assert_eq!(
            hex_encode(&sha256(b"abc")),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
        assert_eq!(hex_encode(&md5(b"abc")), "900150983cd24fb0d6963f7d28e17f72");
        assert_eq!(hex_encode(&md5(b"")), "d41d8cd98f00b204e9800998ecf8427e");
        assert_eq!(
            mongo_sha1_password("scram256", "dummy-pass-256"),
            "e5f2bac50e85f6a08e894ac3270f7949"
        );
        assert_eq!(
            hex_encode(&hmac_sha1(b"key", b"The quick brown fox jumps over the lazy dog")),
            "de7c9b85b8b78aa6bc8a7a36f70a90701c9db4d9"
        );
        assert_eq!(
            hex_encode(&hmac_sha256(b"key", b"The quick brown fox jumps over the lazy dog")),
            "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"
        );
        assert_eq!(
            hex_encode(&pbkdf2_sha1(b"password", b"salt", 1, 20)),
            "0c60c80f961f0e71f3a9b524af6012062fe037a6"
        );
        assert_eq!(
            hex_encode(&pbkdf2_sha1(b"password", b"salt", 4096, 20)),
            "4b007901b765489abead49d926f721d065a429c1"
        );
        assert_eq!(
            hex_encode(&pbkdf2_sha256(b"password", b"salt", 1, 32)),
            "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"
        );
        assert_eq!(
            hex_encode(&pbkdf2_sha256(b"password", b"salt", 4096, 32)),
            "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"
        );
        assert_eq!(base64_encode(b"hello"), "aGVsbG8=");
        assert_eq!(base64_encode(b"f"), "Zg==");
        assert_eq!(base64_encode(b"fo"), "Zm8=");
        assert_eq!(base64_encode(b"foo"), "Zm9v");
        assert_eq!(base64_decode("aGVsbG8=").unwrap(), b"hello");
        assert!(base64_decode("AB*C").is_none());
        assert!(base64_decode("ABC").is_none());
        let bin = [0u8, 0xff, 0x10, 0x80];
        assert_eq!(base64_decode(&base64_encode(&bin)).unwrap(), bin);
        // BSON binary element round-trips through get_binary.
        let doc = doc_of(vec![el_binary("payload", b"n,,n=user,r=x")]);
        assert_eq!(
            get_binary(&doc, "payload").as_deref(),
            Some(b"n,,n=user,r=x".as_slice())
        );
        let doc = doc_of(vec![el_int32("conversationId", 7)]);
        assert_eq!(get_int32(&doc, "conversationId"), Some(7));
    }

    #[test]
    fn scram_sha1_rfc5802_worked_example() {
        // RFC 5802 §5: user "user", password "pencil".
        let mut c = ScramClient::new(
            ScramMechanism::Sha1,
            "user".to_string(),
            b"pencil".to_vec(),
            "fyko+d2lbbFgONRv9qkxdawL",
        );
        assert_eq!(c.client_first(), "n,,n=user,r=fyko+d2lbbFgONRv9qkxdawL");
        c.server_first(
            "r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,s=QSXCR+Q6sek8bf92,i=4096",
        )
        .expect("server-first parses");
        assert_eq!(
            c.client_final(),
            "c=biws,r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,p=v0X8v3Bz2T0CJGbJQyF0X+HI4Ts="
        );
        assert!(c.server_final("v=rmF9pqV8S7suAoZWja4dJRkFsKQ=").is_ok());
        // Rejections: bad server signature, server error, foreign
        // nonce, bad salt.
        let mut bad = ScramClient::new(
            ScramMechanism::Sha1,
            "user".to_string(),
            b"pencil".to_vec(),
            "fyko+d2lbbFgONRv9qkxdawL",
        );
        bad.server_first(
            "r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,s=QSXCR+Q6sek8bf92,i=4096",
        )
        .unwrap();
        assert!(bad.server_final("v=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=").is_err());
        assert!(bad.server_final("e=invalid-proof").is_err());
        let mut foreign = ScramClient::new(
            ScramMechanism::Sha1,
            "user".to_string(),
            b"pencil".to_vec(),
            "fyko+d2lbbFgONRv9qkxdawL",
        );
        assert!(foreign
            .server_first("r=OTHER_NONCE,s=QSXCR+Q6sek8bf92,i=4096")
            .is_err());
        let mut badsalt = ScramClient::new(
            ScramMechanism::Sha1,
            "user".to_string(),
            b"pencil".to_vec(),
            "fyko+d2lbbFgONRv9qkxdawL",
        );
        assert!(badsalt
            .server_first("r=fyko+d2lbbFgONRv9qkxdawL,s=!!!,i=4096")
            .is_err());
    }

    #[test]
    fn scram_sha256_rfc7677_worked_example() {
        // RFC 7677 §3: user "user", password "pencil". (The server
        // nonce contains ')' and '$' — the parser must not choke.)
        let mut c = ScramClient::new(
            ScramMechanism::Sha256,
            "user".to_string(),
            b"pencil".to_vec(),
            "rOprNGfwEbeRWgbNEkqO",
        );
        assert_eq!(c.client_first(), "n,,n=user,r=rOprNGfwEbeRWgbNEkqO");
        c.server_first(
            "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096",
        )
        .expect("server-first parses");
        assert_eq!(
            c.client_final(),
            "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ="
        );
        assert!(c
            .server_final("v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=")
            .is_ok());
    }

    #[test]
    fn redis_url_parse_via_connect_failure() {
        // A bad URL shape fails before any socket is opened.
        assert!(RedisClient::connect("http://127.0.0.1:6379").is_err());
        assert!(RedisClient::connect("redis://127.0.0.1:notaport").is_err());
    }

    /// Live round-trip against a real mongod. Skipped unless
    /// PARLAY_TEST_MONGO_URI is set (e.g. mongodb://127.0.0.1:27099/parlay_test).
    #[test]
    fn live_mongo_round_trip() {
        let uri = match std::env::var("PARLAY_TEST_MONGO_URI") {
            Ok(u) if !u.is_empty() => u,
            _ => {
                eprintln!("SKIP live_mongo_round_trip (PARLAY_TEST_MONGO_URI unset)");
                return;
            }
        };
        let mut c = match MongoClient::connect(&uri) {
            Ok(c) => c,
            Err(e) => {
                eprintln!("SKIP live_mongo_round_trip (no mongod: {})", e);
                return;
            }
        };
        let key = "kv:a11-test-room";
        let val = "{\"version\":1,\"chat_id\":4242}";
        c.upsert_value(key, val).expect("upsert");
        assert_eq!(c.find_value(key).expect("find").as_deref(), Some(val));
        let ks = c.keys_with_prefix("kv:a11-test-").expect("keys");
        assert!(ks.iter().any(|k| k == key), "keys: {:?}", ks);
        c.delete_key(key).expect("delete");
        assert_eq!(c.find_value(key).expect("find after delete"), None);
        // Unknown key reads None, never an error.
        assert_eq!(
            c.find_value("kv:a11-no-such-key").expect("find missing"),
            None
        );
    }

    /// Live SCRAM-SHA-256 round-trip against a real auth-enabled
    /// mongod. Skipped unless PARLAY_TEST_MONGO_AUTH_URI is set
    /// (e.g. mongodb://scram256:dummy-pass-256@127.0.0.1:27096/parlay_test).
    #[test]
    fn live_mongo_auth_sha256_round_trip() {
        let uri = match std::env::var("PARLAY_TEST_MONGO_AUTH_URI") {
            Ok(u) if !u.is_empty() => u,
            _ => {
                eprintln!("SKIP live_mongo_auth_sha256_round_trip (PARLAY_TEST_MONGO_AUTH_URI unset)");
                return;
            }
        };
        let mut c = match MongoClient::connect(&uri) {
            Ok(c) => c,
            Err(e) => {
                panic!("auth connect failed: {}", e);
            }
        };
        let key = "kv:r-scram256-room";
        let val = "{\"version\":1,\"chat_id\":4242}";
        c.upsert_value(key, val).expect("upsert");
        assert_eq!(c.find_value(key).expect("find").as_deref(), Some(val));
        c.delete_key(key).expect("delete");
        assert_eq!(c.find_value(key).expect("find after delete"), None);
    }

    /// Live SCRAM-SHA-1 round-trip against a real auth-enabled
    /// mongod, via automatic negotiation (SHA-256 saslStart fails for
    /// a SHA-1-only user, the client falls back) and via a forced
    /// ?authMechanism=SCRAM-SHA-1 URI. Skipped unless
    /// PARLAY_TEST_MONGO_AUTH_SHA1_URI is set.
    #[test]
    fn live_mongo_auth_sha1_round_trip() {
        let uri = match std::env::var("PARLAY_TEST_MONGO_AUTH_SHA1_URI") {
            Ok(u) if !u.is_empty() => u,
            _ => {
                eprintln!("SKIP live_mongo_auth_sha1_round_trip (PARLAY_TEST_MONGO_AUTH_SHA1_URI unset)");
                return;
            }
        };
        // Negotiated: SHA-256 first, falls back to SHA-1.
        let mut c = MongoClient::connect(&uri)
            .unwrap_or_else(|e| panic!("SHA-1-only user negotiated connect failed: {}", e));
        let key = "kv:r-scram1-room";
        c.upsert_value(key, "v1").expect("upsert");
        assert_eq!(c.find_value(key).expect("find").as_deref(), Some("v1"));
        c.delete_key(key).expect("delete");
        // Forced SHA-1.
        let forced = format!("{}?authMechanism=SCRAM-SHA-1", uri);
        let mut c2 = MongoClient::connect(&forced)
            .unwrap_or_else(|e| panic!("forced SCRAM-SHA-1 connect failed: {}", e));
        c2.upsert_value(key, "v2").expect("upsert");
        assert_eq!(c2.find_value(key).expect("find").as_deref(), Some("v2"));
        c2.delete_key(key).expect("delete");
    }

    /// Live: a wrong password fails honestly at connect (server
    /// AuthenticationFailed), never a silent success. Skipped unless
    /// PARLAY_TEST_MONGO_AUTH_WRONG_URI is set.
    #[test]
    fn live_mongo_auth_wrong_password_fails() {
        let uri = match std::env::var("PARLAY_TEST_MONGO_AUTH_WRONG_URI") {
            Ok(u) if !u.is_empty() => u,
            _ => {
                eprintln!("SKIP live_mongo_auth_wrong_password_fails (PARLAY_TEST_MONGO_AUTH_WRONG_URI unset)");
                return;
            }
        };
        let err = match MongoClient::connect(&uri) {
            Ok(_) => panic!("wrong password must fail to connect"),
            Err(e) => e,
        };
        assert!(
            err.contains("Authentication failed") || err.contains("SCRAM authentication failed"),
            "honest auth failure, got: {}",
            err
        );
    }

    /// Live round-trip against a real redis. Skipped unless
    /// PARLAY_TEST_REDIS_URL is set (e.g. redis://127.0.0.1:6399/0).
    #[test]
    fn live_redis_round_trip() {
        let url = match std::env::var("PARLAY_TEST_REDIS_URL") {
            Ok(u) if !u.is_empty() => u,
            _ => {
                eprintln!("SKIP live_redis_round_trip (PARLAY_TEST_REDIS_URL unset)");
                return;
            }
        };
        let mut c = match RedisClient::connect(&url) {
            Ok(c) => c,
            Err(e) => {
                eprintln!("SKIP live_redis_round_trip (no redis: {})", e);
                return;
            }
        };
        let key = "parlay:kv:a11-test-room";
        c.set(key, "v1").expect("set");
        assert_eq!(c.get(key).expect("get").as_deref(), Some("v1"));
        let ks = c.keys("parlay:kv:a11-test-*").expect("keys");
        assert!(ks.iter().any(|k| k == key), "keys: {:?}", ks);
        c.del(key).expect("del");
        assert_eq!(c.get(key).expect("get after del"), None);
    }
}
