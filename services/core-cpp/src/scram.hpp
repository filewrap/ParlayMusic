// scram.hpp — self-contained crypto primitives (no OpenSSL).
//
// The core links no external libraries (see CMakeLists.txt) and the
// Docker build must stay offline, so the SHA-1 / SHA-256 / HMAC /
// PBKDF2 primitives live here, written from the specs (FIPS 180-4,
// RFC 2104, RFC 2898) with no dependency. Live callers: sha256
// digests track ids into tokens (ui_texts.hpp) and base64Encode
// encodes TDLib `bytes` fields (dispatcher.cpp); the rest of the
// primitives are the module's tested public surface for future
// crypto needs (token signing, key derivation).
//
// History: this file also carried the SCRAM client side (RFC 5802 /
// RFC 7677) for the old database client; that client was deleted in
// Phase 4 (Q1) with the database backends, and the SCRAM-only
// helpers (MD5, the SCRAM-SHA-1 password derivation, the Client
// class) went with it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace parlay::scram {

// --- Hash primitives (raw digest bytes out) ---
std::string sha1(const std::string& data);    // 20 bytes
std::string sha256(const std::string& data);  // 32 bytes

// --- HMAC (RFC 2104) ---
std::string hmacSha1(const std::string& key, const std::string& msg);
std::string hmacSha256(const std::string& key, const std::string& msg);

// --- PBKDF2-HMAC (RFC 2898). Returns dk_len bytes. ---
std::string pbkdf2Sha1(const std::string& password, const std::string& salt,
                       int iterations, std::size_t dk_len);
std::string pbkdf2Sha256(const std::string& password, const std::string& salt,
                         int iterations, std::size_t dk_len);

// --- Base64 (RFC 4648, standard alphabet, padded) ---
std::string base64Encode(const std::string& data);
// False on invalid input (bad alphabet / bad padding / bad length).
bool base64Decode(const std::string& in, std::string& out);

// --- Random nonce: base64 of n_bytes from /dev/urandom ---
// (falls back to a time+pid+counter mix if urandom is unavailable).
std::string randomNonceBase64(std::size_t n_bytes = 18);

}  // namespace parlay::scram
