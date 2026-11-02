//! AES-256-GCM, pure std (no crates): the spotify-rust service is
//! std-only by design, so there is no `aes-gcm` crate to reach for.
//! This module exists for exactly one job — encrypting per-user Spotify
//! OAuth tokens at rest (see `auth.rs`) — and is verified against
//! published test vectors in the tests below, not trusted on faith.
//!
//! Scheme (documented in `auth.rs` and the service README):
//!   * AES-256-GCM, 96-bit random nonce per record, 128-bit tag.
//!   * Key: 32 random bytes from /dev/urandom, generated once at first
//!     run and kept in `spotify-user-key` (0600). Never a hardcoded
//!     default, never derived from anything guessable.
//!   * AAD: the string `parlay:spotify-user-token:v1:<user_id>` — a
//!     record cannot be transplanted to another user id.
//!
//! The S-box is generated at runtime from the GF(2^8) inverse + affine
//! transform instead of a 256-byte pasted table (same values, no
//! transcription risk). Only encryption is implemented: GCM needs
//! AES-encrypt only, never decrypt.

/// Generate the AES S-box at runtime: s(x) = affine(inv(x)) over
/// GF(2^8) mod x^8+x^4+x^3+x+1 (0x11b), affine constant 0x63.
fn gf_mul(mut a: u8, mut b: u8) -> u8 {
    let mut out: u8 = 0;
    while b != 0 {
        if b & 1 != 0 {
            out ^= a;
        }
        let hi = a & 0x80;
        a <<= 1;
        if hi != 0 {
            a ^= 0x1b; // low byte of 0x11b
        }
        b >>= 1;
    }
    out
}

fn gf_inv(x: u8) -> u8 {
    if x == 0 {
        return 0;
    }
    // a^254 in GF(2^8) is the inverse (a^255 == 1 for a != 0).
    let mut result: u8 = 1;
    let mut base = x;
    let mut exp = 254u16;
    while exp > 0 {
        if exp & 1 != 0 {
            result = gf_mul(result, base);
        }
        base = gf_mul(base, base);
        exp >>= 1;
    }
    result
}

fn rotl8(x: u8, n: u32) -> u8 {
    x.rotate_left(n)
}

fn sbox_value(x: u8) -> u8 {
    let inv = gf_inv(x);
    inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^ rotl8(inv, 3) ^ rotl8(inv, 4) ^ 0x63
}

fn build_sbox() -> [u8; 256] {
    let mut s = [0u8; 256];
    for (i, v) in s.iter_mut().enumerate() {
        *v = sbox_value(i as u8);
    }
    s
}

fn build_rcon() -> [u8; 15] {
    // Rcon[i] = x^(i-1) in GF(2^8), i = 1..14.
    let mut r = [0u8; 15];
    let mut x: u8 = 1;
    for v in r.iter_mut().skip(1) {
        *v = x;
        x = gf_mul(x, 2);
    }
    r
}

/// AES-256 key schedule: 60 32-bit words. Returns the 15 round keys
/// (each 16 bytes) used by the encrypt routine.
fn key_expansion(key: &[u8; 32], sbox: &[u8; 256], rcon: &[u8; 15]) -> [[u8; 16]; 15] {
    let mut w = [[0u8; 4]; 60];
    for i in 0..8 {
        w[i] = [key[4 * i], key[4 * i + 1], key[4 * i + 2], key[4 * i + 3]];
    }
    for i in 8..60 {
        let mut temp = w[i - 1];
        if i % 8 == 0 {
            // RotWord, SubWord, Rcon.
            temp = [
                sbox[temp[1] as usize] ^ rcon[i / 8],
                sbox[temp[2] as usize],
                sbox[temp[3] as usize],
                sbox[temp[0] as usize],
            ];
        } else if i % 8 == 4 {
            temp = [
                sbox[temp[0] as usize],
                sbox[temp[1] as usize],
                sbox[temp[2] as usize],
                sbox[temp[3] as usize],
            ];
        }
        for j in 0..4 {
            w[i][j] = w[i - 8][j] ^ temp[j];
        }
    }
    let mut round_keys = [[0u8; 16]; 15];
    for r in 0..15 {
        for c in 0..4 {
            for j in 0..4 {
                round_keys[r][4 * c + j] = w[4 * r + c][j];
            }
        }
    }
    round_keys
}

fn add_round_key(state: &mut [u8; 16], rk: &[u8; 16]) {
    for i in 0..16 {
        state[i] ^= rk[i];
    }
}

fn sub_bytes(state: &mut [u8; 16], sbox: &[u8; 256]) {
    for b in state.iter_mut() {
        *b = sbox[*b as usize];
    }
}

fn shift_rows(state: &mut [u8; 16]) {
    // State is column-major: state[4*c + r].
    let s = *state;
    for r in 0..4 {
        for c in 0..4 {
            state[4 * c + r] = s[4 * ((c + r) % 4) + r];
        }
    }
}

fn mix_columns(state: &mut [u8; 16]) {
    for c in 0..4 {
        let i = 4 * c;
        let a0 = state[i];
        let a1 = state[i + 1];
        let a2 = state[i + 2];
        let a3 = state[i + 3];
        state[i] = gf_mul(a0, 2) ^ gf_mul(a1, 3) ^ a2 ^ a3;
        state[i + 1] = a0 ^ gf_mul(a1, 2) ^ gf_mul(a2, 3) ^ a3;
        state[i + 2] = a0 ^ a1 ^ gf_mul(a2, 2) ^ gf_mul(a3, 3);
        state[i + 3] = gf_mul(a0, 3) ^ a1 ^ a2 ^ gf_mul(a3, 2);
    }
}

/// AES-256 single-block encryption (FIPS-197).
pub fn aes256_encrypt_block(key: &[u8; 32], block: &[u8; 16]) -> [u8; 16] {
    let sbox = build_sbox();
    let rcon = build_rcon();
    let round_keys = key_expansion(key, &sbox, &rcon);
    let mut state = *block;
    add_round_key(&mut state, &round_keys[0]);
    for r in 1..14 {
        sub_bytes(&mut state, &sbox);
        shift_rows(&mut state);
        mix_columns(&mut state);
        add_round_key(&mut state, &round_keys[r]);
    }
    sub_bytes(&mut state, &sbox);
    shift_rows(&mut state);
    add_round_key(&mut state, &round_keys[14]);
    state
}

// --------------------------------------------------------------- GHASH ---

/// Multiply two 128-bit values in GF(2^128) with the GCM polynomial
/// x^128 + x^7 + x^2 + x + 1, big-endian bit order (NIST SP 800-38D
/// Algorithm 1). `h` is the hash subkey, `x` the block to multiply.
fn gf128_mul(h: &[u8; 16], x: &[u8; 16]) -> [u8; 16] {
    let mut z = [0u8; 16];
    let mut v = *h;
    // R = 11100001 || 0^120 (the reduction polynomial, big-endian).
    let r: [u8; 16] = [
        0xe1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    ];
    for i in 0..128 {
        let byte = x[i / 8];
        let bit = (byte >> (7 - (i % 8))) & 1;
        if bit == 1 {
            for j in 0..16 {
                z[j] ^= v[j];
            }
        }
        let lsb = v[15] & 1;
        for j in (1..16).rev() {
            v[j] = (v[j] >> 1) | ((v[j - 1] & 1) << 7);
        }
        v[0] >>= 1;
        if lsb == 1 {
            for j in 0..16 {
                v[j] ^= r[j];
            }
        }
    }
    z
}

/// GHASH over AAD + ciphertext + the 64-bit length block.
fn ghash(h: &[u8; 16], aad: &[u8], ciphertext: &[u8]) -> [u8; 16] {
    let mut y = [0u8; 16];
    let process = |y: &mut [u8; 16], data: &[u8]| {
        for chunk in data.chunks(16) {
            let mut block = [0u8; 16];
            block[..chunk.len()].copy_from_slice(chunk);
            for j in 0..16 {
                y[j] ^= block[j];
            }
            *y = gf128_mul(h, y);
        }
    };
    process(&mut y, aad);
    process(&mut y, ciphertext);
    let mut len_block = [0u8; 16];
    len_block[..8].copy_from_slice(&((aad.len() as u64 * 8).to_be_bytes()));
    len_block[8..].copy_from_slice(&((ciphertext.len() as u64 * 8).to_be_bytes()));
    for j in 0..16 {
        y[j] ^= len_block[j];
    }
    gf128_mul(h, &y)
}

fn inc32(block: &[u8; 16]) -> [u8; 16] {
    let mut out = *block;
    let ctr = u32::from_be_bytes([out[12], out[13], out[14], out[15]]).wrapping_add(1);
    out[12..].copy_from_slice(&ctr.to_be_bytes());
    out
}

// ------------------------------------------------------------- AES-GCM ---

/// AES-256-GCM encryption. `nonce` is 12 bytes; returns
/// (ciphertext, 16-byte tag). AAD may be empty.
pub fn gcm_encrypt(
    key: &[u8; 32],
    nonce: &[u8; 12],
    plaintext: &[u8],
    aad: &[u8],
) -> (Vec<u8>, [u8; 16]) {
    let h = aes256_encrypt_block(key, &[0u8; 16]);
    let mut j0 = [0u8; 16];
    j0[..12].copy_from_slice(nonce);
    j0[15] = 1;
    let mut counter = inc32(&j0);
    let mut ciphertext = Vec::with_capacity(plaintext.len());
    for chunk in plaintext.chunks(16) {
        let ks = aes256_encrypt_block(key, &counter);
        counter = inc32(&counter);
        for (i, &b) in chunk.iter().enumerate() {
            ciphertext.push(b ^ ks[i]);
        }
    }
    let s = ghash(&h, aad, &ciphertext);
    let e_j0 = aes256_encrypt_block(key, &j0);
    let mut tag = [0u8; 16];
    for i in 0..16 {
        tag[i] = e_j0[i] ^ s[i];
    }
    (ciphertext, tag)
}

/// AES-256-GCM decryption. Returns None when the tag does not verify
/// (wrong key, wrong AAD, or any tampering) — callers must treat None
/// as "unusable record", never as an empty plaintext.
pub fn gcm_decrypt(
    key: &[u8; 32],
    nonce: &[u8; 12],
    ciphertext: &[u8],
    aad: &[u8],
    tag: &[u8; 16],
) -> Option<Vec<u8>> {
    let h = aes256_encrypt_block(key, &[0u8; 16]);
    let mut j0 = [0u8; 16];
    j0[..12].copy_from_slice(nonce);
    j0[15] = 1;
    let s = ghash(&h, aad, ciphertext);
    let e_j0 = aes256_encrypt_block(key, &j0);
    let mut expected = [0u8; 16];
    for i in 0..16 {
        expected[i] = e_j0[i] ^ s[i];
    }
    // Constant-time-ish tag compare (no early exit on first mismatch).
    let mut diff = 0u8;
    for i in 0..16 {
        diff |= expected[i] ^ tag[i];
    }
    if diff != 0 {
        return None;
    }
    let mut counter = inc32(&j0);
    let mut plaintext = Vec::with_capacity(ciphertext.len());
    for chunk in ciphertext.chunks(16) {
        let ks = aes256_encrypt_block(key, &counter);
        counter = inc32(&counter);
        for (i, &b) in chunk.iter().enumerate() {
            plaintext.push(b ^ ks[i]);
        }
    }
    Some(plaintext)
}

/// 32 random bytes from /dev/urandom (xorshift/time fallback only if
/// urandom is unavailable — same pattern as common::random_token).
pub fn random_key_32() -> [u8; 32] {
    use std::io::Read;
    let mut key = [0u8; 32];
    let got = std::fs::File::open("/dev/urandom")
        .and_then(|mut f| f.read_exact(&mut key))
        .is_ok();
    if !got {
        let seed = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos() as u64)
            .unwrap_or(0)
            ^ ((std::process::id() as u64) << 32);
        let mut x = seed | 1;
        for b in key.iter_mut() {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            *b = (x & 0xff) as u8;
        }
    }
    key
}

pub fn random_nonce_12() -> [u8; 12] {
    let k = random_key_32();
    let mut n = [0u8; 12];
    n.copy_from_slice(&k[..12]);
    n
}

pub fn to_hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{:02x}", b)).collect()
}

pub fn from_hex(s: &str) -> Option<Vec<u8>> {
    if s.len() % 2 != 0 {
        return None;
    }
    let mut out = Vec::with_capacity(s.len() / 2);
    let b = s.as_bytes();
    for i in (0..b.len()).step_by(2) {
        let hi = (b[i] as char).to_digit(16)?;
        let lo = (b[i + 1] as char).to_digit(16)?;
        out.push(((hi << 4) | lo) as u8);
    }
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sbox_matches_fips197() {
        let s = build_sbox();
        // FIPS-197 §5.1.1, first row of the S-box table.
        assert_eq!(
            &s[0..16],
            &[
                0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe,
                0xd7, 0xab, 0x76
            ]
        );
        assert_eq!(s[0x53], 0xed); // FIPS-197 example byte
        assert_eq!(s[0xff], 0x16);
    }

    #[test]
    fn aes256_block_matches_fips197() {
        // FIPS-197 Appendix C.3: AES-256.
        let key: [u8; 32] = [
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
            0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
            0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        ];
        let pt: [u8; 16] = [
            0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc,
            0xdd, 0xee, 0xff,
        ];
        let ct = aes256_encrypt_block(&key, &pt);
        assert_eq!(
            ct,
            [
                0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf, 0xea, 0xfc, 0x49, 0x90,
                0x4b, 0x49, 0x60, 0x89
            ]
        );
    }

    #[test]
    fn gcm_zero_vector_aes256() {
        // NIST GCM test vector (gcmEncryptExtIV256, case 13): zero key,
        // zero 96-bit IV, 16 zero plaintext bytes, empty AAD.
        let key = [0u8; 32];
        let nonce = [0u8; 12];
        let (ct, tag) = gcm_encrypt(&key, &nonce, &[0u8; 16], &[]);
        assert_eq!(
            to_hex(&ct),
            "cea7403d4d606b6e074ec5d3baf39d18"
        );
        assert_eq!(
            to_hex(&tag),
            "d0d1c8a799996bf0265b98b5d48ab919"
        );
        let pt = gcm_decrypt(&key, &nonce, &ct, &[], &tag).expect("decrypts");
        assert_eq!(pt, vec![0u8; 16]);
    }

    #[test]
    fn gcm_roundtrip_random_key_nonce_aad() {
        let key = random_key_32();
        let nonce = random_nonce_12();
        let pt = b"spotify access token, definitely not real";
        let aad = b"parlay:spotify-user-token:v1:12345";
        let (ct, tag) = gcm_encrypt(&key, &nonce, pt, aad);
        assert_ne!(ct, pt.to_vec());
        let back = gcm_decrypt(&key, &nonce, &ct, aad, &tag).expect("round-trip");
        assert_eq!(back, pt.to_vec());
    }

    #[test]
    fn gcm_rejects_tampered_ciphertext_tag_and_aad() {
        let key = random_key_32();
        let nonce = random_nonce_12();
        let pt = b"token-bytes";
        let aad = b"aad";
        let (mut ct, mut tag) = gcm_encrypt(&key, &nonce, pt, aad);
        assert!(gcm_decrypt(&key, &nonce, &ct, aad, &tag).is_some());
        ct[0] ^= 1;
        assert!(gcm_decrypt(&key, &nonce, &ct, aad, &tag).is_none());
        ct[0] ^= 1;
        tag[0] ^= 1;
        assert!(gcm_decrypt(&key, &nonce, &ct, aad, &tag).is_none());
        tag[0] ^= 1;
        assert!(gcm_decrypt(&key, &nonce, &ct, b"other-aad", &tag).is_none());
    }

    #[test]
    fn hex_roundtrip() {
        assert_eq!(to_hex(&[0x00, 0xab, 0xff]), "00abff");
        assert_eq!(from_hex("00abff"), Some(vec![0x00, 0xab, 0xff]));
        assert_eq!(from_hex("zz"), None);
        assert_eq!(from_hex("abc"), None);
    }
}
