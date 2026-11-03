// scram.hpp — self-contained crypto for MongoDB SCRAM authentication.
//
// The core links no external libraries (see CMakeLists.txt): no libmongoc,
// no OpenSSL at link time, and the Docker build must stay offline. So the
// SHA-1 / SHA-256 / HMAC / PBKDF2 primitives SCRAM needs live here, written
// from the specs (FIPS 180-4, RFC 2104, RFC 2898) with no dependency.
//
// SCRAM itself (RFC 5802 for SHA-1, RFC 7677 for SHA-256) runs over the
// hand-rolled MongoDB wire client in store.cpp: saslStart / saslContinue
// OP_MSG commands against the server's authSource database. Channel
// binding is omitted (client sends "n,," -> "c=biws"), exactly as the
// drivers do when no TLS channel binding is available.
//
// Limitation, stated honestly: passwords are used as UTF-8 bytes with no
// SASLprep normalization (RFC 4013). For ASCII passwords — the realistic
// case for MONGO_URI — SASLprep is the identity function, so this is
// exact. A non-ASCII password could authenticate differently than a
// SASLprep-applying driver; that case is out of scope for this client.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace parlay::scram {

// --- Hash primitives (raw digest bytes out) ---
std::string sha1(const std::string& data);    // 20 bytes
std::string sha256(const std::string& data);  // 32 bytes

// --- MD5 (RFC 1321, raw 16 bytes). Used only for mongoSha1Password. ---
std::string md5(const std::string& data);

// MongoDB's SCRAM-SHA-1 password derivation: the server stores SCRAM-SHA-1
// credentials derived from hex(MD5(username + ":mongo:" + password))
// (the legacy MONGODB-CR digest), NOT from the raw password — verified
// live against mongod 8.0.4 (raw password fails, this digest
// authenticates). SCRAM-SHA-256 uses the raw password. The scram::Client
// below is pure RFC 5802/7677 and takes the effective password as given;
// the MongoDB wire client (store.cpp) applies this transform for SHA-1.
std::string mongoSha1Password(const std::string& username,
                              const std::string& password);

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

enum class Mechanism { kSha1, kSha256 };
const char* mechanismName(Mechanism m);
// Parses "SCRAM-SHA-1" / "SCRAM-SHA-256" (exact, case-sensitive).
bool parseMechanism(const std::string& name, Mechanism& out);

// --- SCRAM client side (RFC 5802 / RFC 7677) ------------------------------
// One instance = one authentication attempt. The caller drives the
// saslStart / saslContinue exchange:
//
//   saslStart payload = base64(clientFirst())   [raw bytes, not base64,
//                                                on the MongoDB wire:
//                                                BinData(0, clientFirst())]
//   serverFirst(server-first payload bytes)     -> then clientFinal()
//   saslContinue payload = BinData(0, clientFinal())
//   serverFinal(server-final payload bytes)     -> verifies ServerSignature
class Client {
public:
    // nonce_b64: fixed client nonce (base64 text) for tests; empty (the
    // default) generates a random nonce. The nonce is used verbatim as
    // the SCRAM "r=" value.
    Client(Mechanism mechanism, std::string username, std::string password,
           std::string nonce_b64 = "");

    // Full client-first-message: "n,,n=<user>,r=<nonce>".
    // (The "n,," is the GS2 header: no channel binding, no authzid.)
    std::string clientFirst() const;
    // Bare part after the GS2 header: "n=<user>,r=<nonce>".
    const std::string& clientFirstBare() const { return client_first_bare_; }

    // Parse the server-first-message ("r=<nonce>,s=<salt>,i=<iters>").
    // Verifies the server nonce starts with our client nonce, then
    // derives the salted password (PBKDF2). False + error_out on
    // malformed input, nonce mismatch, or bad iteration count.
    bool serverFirst(const std::string& server_first, std::string& error_out);

    // Client-final-message: "c=biws,r=<full nonce>,p=<base64 proof>".
    // ("biws" is base64("n,,").) Requires serverFirst() to have run.
    std::string clientFinal() const;

    // Verify the server-final-message ("v=<base64 ServerSignature>" on
    // success, "e=<error>" on failure). False + error_out on mismatch,
    // server error, or malformed input. Requires serverFirst() to have run.
    bool serverFinal(const std::string& server_final, std::string& error_out);

    Mechanism mechanism() const { return mechanism_; }
    const std::string& username() const { return username_; }
    // Full (client+server) nonce, valid after serverFirst().
    const std::string& fullNonce() const { return full_nonce_; }

private:
    Mechanism mechanism_;
    std::string username_;
    std::string password_;
    std::string client_first_bare_;
    std::string full_nonce_;
    std::string salted_password_;  // set by serverFirst()
    std::string auth_message_;     // set by serverFirst()
    bool have_server_first_ = false;
};

}  // namespace parlay::scram
