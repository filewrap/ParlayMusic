// scram_test.cpp — validates the self-contained crypto primitives in
// scram.{hpp,cpp} (SHA-1 / SHA-256 / HMAC / PBKDF2 / base64). No
// servers are needed: every check is a known-answer vector.
//
// History: this file used to validate the SCRAM client side
// (RFC 5802 / RFC 7677) and the hand-rolled database wire client
// against a real auth-enabled database. That client was deleted in
// Phase 4 (Q1) with the database backends, and the SCRAM-only
// helpers (MD5, the SCRAM-SHA-1 password derivation, the Client
// class) went with it. What remains is the crypto-utility module
// the core still uses (sha256 for track tokens, base64 for TDLib
// `bytes` fields).
//
// Usage: ./scram_test
// Exit 0 = all checks pass. Prints PASS/FAIL per check.
#include <cstdio>
#include <iostream>
#include <string>

#include "scram.hpp"

static int failures = 0;
static int checks = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        ++checks;                                                           \
        if (cond) {                                                         \
            std::cout << "PASS " << msg << "\n";                           \
        } else {                                                            \
            std::cout << "FAIL " << msg << "\n";                           \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

static std::string hexOf(const std::string& raw) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (unsigned char c : raw) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0xf]);
    }
    return out;
}

int main() {
    // --- Crypto primitives (no server) ---
    // Vectors verified against Python hashlib/hmac (RFC 6070 PBKDF2).
    {
        CHECK(hexOf(parlay::scram::sha1("abc")) ==
                  "a9993e364706816aba3e25717850c26c9cd0d89d",
              "sha1(abc)");
        CHECK(hexOf(parlay::scram::sha256("abc")) ==
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f2"
                  "0015ad",
              "sha256(abc)");
        CHECK(hexOf(parlay::scram::sha256("")) ==
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991"
                  "b7852b855",
              "sha256(empty)");
        CHECK(hexOf(parlay::scram::hmacSha1(
                  "key", "The quick brown fox jumps over the lazy dog")) ==
                  "de7c9b85b8b78aa6bc8a7a36f70a90701c9db4d9",
              "hmac-sha1 fox");
        CHECK(hexOf(parlay::scram::hmacSha256(
                  "key", "The quick brown fox jumps over the lazy dog")) ==
                  "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d"
                  "1a3cd8",
              "hmac-sha256 fox");
        CHECK(hexOf(parlay::scram::pbkdf2Sha1("password", "salt", 1, 20)) ==
                  "0c60c80f961f0e71f3a9b524af6012062fe037a6",
              "pbkdf2-sha1 c=1 (RFC 6070)");
        CHECK(hexOf(parlay::scram::pbkdf2Sha1("password", "salt", 4096,
                                              20)) ==
                  "4b007901b765489abead49d926f721d065a429c1",
              "pbkdf2-sha1 c=4096 (RFC 6070)");
        CHECK(hexOf(parlay::scram::pbkdf2Sha256("password", "salt", 1,
                                                32)) ==
                  "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb7"
                  "0be17b",
              "pbkdf2-sha256 c=1");
        CHECK(hexOf(parlay::scram::pbkdf2Sha256("password", "salt", 4096,
                                                32)) ==
                  "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873a"
                  "a98134a",
              "pbkdf2-sha256 c=4096");
        CHECK(parlay::scram::base64Encode("hello") == "aGVsbG8=",
              "base64 encode hello");
        CHECK(parlay::scram::base64Encode("f") == "Zg==" &&
                  parlay::scram::base64Encode("fo") == "Zm8=" &&
                  parlay::scram::base64Encode("foo") == "Zm9v",
              "base64 encode padding");
        std::string dec;
        CHECK(parlay::scram::base64Decode("aGVsbG8=", dec) &&
                  dec == "hello",
              "base64 decode hello");
        CHECK(parlay::scram::base64Decode(std::string("\x00\x01\xff", 3),
                                          dec) == false,
              "base64 decode rejects bad alphabet");
        CHECK(parlay::scram::base64Decode("ABC", dec) == false,
              "base64 decode rejects bad length");
        std::string bin("\x00\xff\x10\x80", 4);
        CHECK(parlay::scram::base64Decode(
                  parlay::scram::base64Encode(bin), dec) && dec == bin,
              "base64 round-trip binary");
        // Random nonces: two draws differ and decode to the right size.
        std::string n1 = parlay::scram::randomNonceBase64();
        std::string n2 = parlay::scram::randomNonceBase64();
        std::string raw;
        CHECK(n1 != n2 && parlay::scram::base64Decode(n1, raw) &&
                  raw.size() == 18,
              "random nonce: unique, 18 bytes");
    }

    std::cout << checks << " checks, "
              << (failures == 0 ? "ALL PASS"
                                : "FAILURES: " + std::to_string(failures))
              << "\n";
    return failures == 0 ? 0 : 1;
}
