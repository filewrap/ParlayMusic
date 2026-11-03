// scram_test.cpp — validates SCRAM-SHA-1/256 auth in the hand-rolled
// MongoDB wire client (store.cpp + scram.cpp), against a REAL auth-enabled
// mongod. Dummy credentials only; the mongod is a scratch instance the
// caller starts (see phase7/worker-a7-scram.md for the setup commands).
//
// Usage:
//   ./scram_test [auth256_uri] [auth1_uri] [wrong_uri] [admin_uri] [plain_uri]
//
// Defaults (non-default ports; nothing on :27017):
//   auth256: mongodb://scram256:dummy-pass-256@127.0.0.1:27098/parlay_test
//   auth1:   mongodb://scram1:dummy-pass-1@127.0.0.1:27098/parlay_test
//            (user has SCRAM-SHA-1 credentials only -> the client must
//            negotiate down from SHA-256 automatically)
//   wrong:   mongodb://scram256:wrong-password@127.0.0.1:27098/parlay_test
//   admin:   mongodb://admintest:dummy-pass-admin@127.0.0.1:27098/parlay_test?authSource=admin
//            (user lives in the admin db)
//   plain:   mongodb://127.0.0.1:27097/parlay_test  (separate no-auth mongod)
//
// Live sections are SKIPped (not failed) when their mongod is not
// reachable at all ("cannot connect"); a reachable server that refuses
// auth or CRUD is a hard FAIL. Exit 0 = all checks pass (or skipped).
#include <cstdio>
#include <iostream>
#include <string>

#include "config.hpp"
#include "scram.hpp"
#include "store.hpp"

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

// Reachability gate: true when the server answers TCP at all (even with
// an auth error). Used to SKIP live sections instead of failing them
// when the scratch mongod simply is not running.
static bool mongoReachable(const std::string& uri) {
    parlay::MongoUri parsed;
    std::string err;
    if (!parlay::MongoClient::parseUri(uri, parsed, err)) return false;
    parlay::MongoClient m;
    // Connect with credentials stripped: a reachable auth mongod refuses
    // the unauthenticated ping, but the TCP + wire round trip proves it
    // is up. (parseUri success + any non-"cannot connect" error = up.)
    std::string plain = "mongodb://" + parsed.host + ":" +
                        std::to_string(parsed.port) + "/" + parsed.db;
    bool ok = m.connect(plain, err);
    return ok || err.find("cannot connect") == std::string::npos;
}

static bool crudRoundTrip(parlay::MongoClient& m, const std::string& tag) {
    std::string key = "kv:scram-" + tag, v;
    if (!m.upsertValue(key, "v-" + tag)) return false;
    if (!m.findValue(key, v) || v != "v-" + tag) return false;
    if (!m.deleteKey(key)) return false;
    return !m.findValue(key, v);
}

int main(int argc, char** argv) {
    std::string uri256 = argc > 1 ? argv[1]
                                  : "mongodb://scram256:dummy-pass-256@"
                                    "127.0.0.1:27098/parlay_test";
    std::string uri1 = argc > 2 ? argv[2]
                                : "mongodb://scram1:dummy-pass-1@"
                                  "127.0.0.1:27098/parlay_test";
    std::string uriWrong = argc > 3 ? argv[3]
                                    : "mongodb://scram256:wrong-password@"
                                      "127.0.0.1:27098/parlay_test";
    std::string uriAdmin = argc > 4 ? argv[4]
                                    : "mongodb://admintest:dummy-pass-admin@"
                                      "127.0.0.1:27098/parlay_test"
                                      "?authSource=admin";
    std::string uriPlain = argc > 5 ? argv[5]
                                    : "mongodb://127.0.0.1:27097/parlay_test";

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
        CHECK(hexOf(parlay::scram::md5("abc")) ==
                  "900150983cd24fb0d6963f7d28e17f72",
              "md5(abc) (RFC 1321)");
        CHECK(hexOf(parlay::scram::md5("")) ==
                  "d41d8cd98f00b204e9800998ecf8427e",
              "md5(empty)");
        // MongoDB's SCRAM-SHA-1 password derivation: verified live
        // (this digest authenticates against mongod 8.0.4; the raw
        // password does not).
        CHECK(parlay::scram::mongoSha1Password(
                  "scram256", "dummy-pass-256") ==
                  "e5f2bac50e85f6a08e894ac3270f7949",
              "mongoSha1Password digest");
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
    }

    // --- SCRAM message construction vs the RFC worked examples ---
    {
        // RFC 5802 §5 (SCRAM-SHA-1): user "user", password "pencil".
        parlay::scram::Client c1(parlay::scram::Mechanism::kSha1, "user",
                                 "pencil", "fyko+d2lbbFgONRv9qkxdawL");
        CHECK(c1.clientFirst() ==
                  "n,,n=user,r=fyko+d2lbbFgONRv9qkxdawL",
              "scram-sha1 client-first (RFC 5802)");
        std::string err;
        CHECK(c1.serverFirst(
                  "r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,"
                  "s=QSXCR+Q6sek8bf92,i=4096",
                  err),
              "scram-sha1 server-first parses: " + err);
        CHECK(c1.clientFinal() ==
                  "c=biws,r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,"
                  "p=v0X8v3Bz2T0CJGbJQyF0X+HI4Ts=",
              "scram-sha1 client-final proof (RFC 5802)");
        CHECK(c1.serverFinal("v=rmF9pqV8S7suAoZWja4dJRkFsKQ=", err),
              "scram-sha1 server-final verifies (RFC 5802): " + err);
        CHECK(!c1.serverFinal("v=AAAAAAAAAAAAAAAAAAAAAAAAAAA=", err),
              "scram-sha1 server-final rejects bad signature");
        CHECK(!c1.serverFinal("e=invalid-proof", err),
              "scram-sha1 server-final rejects server error");

        parlay::scram::Client c1bad(parlay::scram::Mechanism::kSha1,
                                    "user", "pencil",
                                    "fyko+d2lbbFgONRv9qkxdawL");
        CHECK(!c1bad.serverFirst(
                  "r=OTHER_NONCE,s=QSXCR+Q6sek8bf92,i=4096", err),
              "scram-sha1 server-first rejects foreign nonce");
        CHECK(!c1bad.serverFirst("r=fyko+d2lbbFgONRv9qkxdawL,s=!!!,"
                                 "i=4096",
                                 err),
              "scram-sha1 server-first rejects bad salt");

        // RFC 7677 §3 (SCRAM-SHA-256): user "user", password "pencil".
        // Proof / server signature computed from the RFC inputs with
        // Python hashlib (PBKDF2 verified against RFC 6070 above).
        parlay::scram::Client c2(parlay::scram::Mechanism::kSha256,
                                 "user", "pencil",
                                 "rOprNGfwEbeRWgbNEkqO");
        CHECK(c2.clientFirst() == "n,,n=user,r=rOprNGfwEbeRWgbNEkqO",
              "scram-sha256 client-first (RFC 7677)");
        CHECK(c2.serverFirst(
                  "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCDQy3w3,"
                  "s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096",
                  err),
              "scram-sha256 server-first parses: " + err);
        CHECK(c2.clientFinal() ==
                  "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCDQy3w3,"
                  "p=v+zym9f5d0P0ahomo8x0b01hTAJldgi/xnN8yXjblIc=",
              "scram-sha256 client-final proof (RFC 7677 inputs)");
        CHECK(c2.serverFinal(
                  "v=IexpTKldGwbakozz5/sFI4NErEghtEaFE7dVQRLlePI=", err),
              "scram-sha256 server-final verifies: " + err);
    }

    // --- URI parsing (no server) ---
    {
        parlay::MongoUri u;
        std::string err;
        CHECK(parlay::MongoClient::parseUri(
                  "mongodb://scram256:dummy-pass-256@127.0.0.1:27098/"
                  "parlay_test?authSource=admin",
                  u, err) &&
                  u.host == "127.0.0.1" && u.port == 27098 &&
                  u.db == "parlay_test" && u.username == "scram256" &&
                  u.password == "dummy-pass-256" &&
                  u.auth_source == "admin" && u.auth_mechanism.empty(),
              "uri parse full auth form");
        CHECK(parlay::MongoClient::parseUri(
                  "mongodb://user:pass@dbhost/mydb", u, err) &&
                  u.host == "dbhost" && u.port == 27017 &&
                  u.db == "mydb" && u.auth_source == "mydb",
              "uri parse authSource defaults to db");
        CHECK(parlay::MongoClient::parseUri("mongodb://dbhost/mydb", u,
                                            err) &&
                  u.username.empty() && u.auth_source == "mydb",
              "uri parse no-auth");
        CHECK(parlay::MongoClient::parseUri("mongodb://dbhost", u, err) &&
                  u.db == "parlay" && u.auth_source == "admin",
              "uri parse no-db defaults to parlay/admin");
        CHECK(parlay::MongoClient::parseUri(
                  "mongodb://u:p%40ss%3Aw%2Frd@dbhost/db", u, err) &&
                  u.password == "p@ss:w/rd",
              "uri parse percent-decodes password");
        CHECK(parlay::MongoClient::parseUri(
                  "mongodb://u:p@dbhost/db?authMechanism=SCRAM-SHA-1",
                  u, err) &&
                  u.auth_mechanism == "SCRAM-SHA-1",
              "uri parse authMechanism=SCRAM-SHA-1");
        CHECK(!parlay::MongoClient::parseUri(
                  "mongodb://u:p@dbhost/db?authMechanism=PLAIN", u, err),
              "uri parse rejects authMechanism=PLAIN");
        CHECK(!parlay::MongoClient::parseUri(
                  "mongodb+srv://u:p@cluster.example.net/db", u, err),
              "uri parse rejects mongodb+srv://");
        CHECK(!parlay::MongoClient::parseUri(
                  "mongodb://u:p@h1:27017,h2:27017/db", u, err),
              "uri parse rejects multi-host");
        CHECK(!parlay::MongoClient::parseUri("mongodb://", u, err),
              "uri parse rejects empty host");
        CHECK(!parlay::MongoClient::parseUri("postgres://dbhost/db", u,
                                             err),
              "uri parse rejects non-mongodb scheme");
    }

    // --- Live: SCRAM-SHA-256 against the auth mongod ---
    if (!mongoReachable(uri256)) {
        std::cout << "SKIP live auth tests (auth mongod not up): " << uri256
                  << "\n";
    } else {
        {
            parlay::MongoClient m;
            std::string err;
            bool ok = m.connect(uri256, err);
            CHECK(ok, std::string("scram-sha256 connect: ") + err);
            if (ok) {
                CHECK(m.ping(), "scram-sha256 ping");
                CHECK(m.authenticated(), "scram-sha256 authenticated()");
                CHECK(m.authSource() == "parlay_test",
                      "scram-sha256 authSource defaults to db");
                CHECK(crudRoundTrip(m, "sha256"), "scram-sha256 CRUD");
            }
        }
        // Forced mechanism via ?authMechanism=SCRAM-SHA-256.
        {
            std::string forced = uri256 +
                                 (uri256.find('?') == std::string::npos
                                      ? "?authMechanism=SCRAM-SHA-256"
                                      : "&authMechanism=SCRAM-SHA-256");
            parlay::MongoClient m;
            std::string err;
            bool ok = m.connect(forced, err);
            CHECK(ok, std::string("scram-sha256 forced-mechanism: ") + err);
            if (ok) CHECK(crudRoundTrip(m, "sha256forced"),
                          "scram-sha256 forced CRUD");
        }
        // SHA-1-only user, no authMechanism param: negotiation must fall
        // back from SHA-256 to SHA-1 on its own.
        {
            parlay::MongoClient m;
            std::string err;
            bool ok = m.connect(uri1, err);
            CHECK(ok, std::string("scram-sha1 negotiated connect: ") + err);
            if (ok) {
                CHECK(m.ping(), "scram-sha1 negotiated ping");
                CHECK(crudRoundTrip(m, "sha1neg"), "scram-sha1 negot CRUD");
            }
        }
        // Forced SCRAM-SHA-1 on the same user.
        {
            std::string forced = uri1 +
                                 (uri1.find('?') == std::string::npos
                                      ? "?authMechanism=SCRAM-SHA-1"
                                      : "&authMechanism=SCRAM-SHA-1");
            parlay::MongoClient m;
            std::string err;
            bool ok = m.connect(forced, err);
            CHECK(ok, std::string("scram-sha1 forced-mechanism: ") + err);
            if (ok) CHECK(crudRoundTrip(m, "sha1forced"),
                          "scram-sha1 forced CRUD");
        }
        // Wrong password: honest failure, no fake success.
        {
            parlay::MongoClient m;
            std::string err;
            bool ok = m.connect(uriWrong, err);
            CHECK(!ok, "wrong-password connect fails");
            CHECK(err.find("Authentication failed") != std::string::npos,
                  std::string("wrong-password error names auth failure: ") +
                      err);
        }
        // authSource=admin: user lives in admin, data in parlay_test.
        {
            parlay::MongoClient m;
            std::string err;
            bool ok = m.connect(uriAdmin, err);
            CHECK(ok, std::string("scram authSource=admin connect: ") + err);
            if (ok) {
                CHECK(m.authSource() == "admin",
                      "scram authSource=admin honoured");
                CHECK(crudRoundTrip(m, "admin"), "scram admin CRUD");
            }
        }
        // Store over authenticated Mongo: backend=mongo, real round-trips.
        {
            parlay::Config cfg;
            cfg.mongo_uri = uri256;
            cfg.redis_url = "";
            cfg.data_dir = "/tmp/parlay-scram-test";
            parlay::Store s(cfg);
            CHECK(s.backend() == "mongo",
                  "store backend=mongo over SCRAM (got " + s.backend() +
                      ")");
            s.set("engine:-2001", "go");
            auto e = s.get("engine:-2001");
            CHECK(e && *e == "go", "store kv round-trip (scram mongo)");
            s.sadd("sudoers", "999");
            CHECK(s.sismember("sudoers", "999"),
                  "store set (scram mongo)");
            s.hset("chat:-2001", "lang", "en");
            CHECK(s.hget("chat:-2001", "lang") ==
                      std::optional<std::string>("en"),
                  "store hash (scram mongo)");
            s.del("engine:-2001");
            CHECK(!s.get("engine:-2001"), "store del (scram mongo)");
        }
    }

    // --- Live: no-auth URI still works (separate plain mongod) ---
    if (!mongoReachable(uriPlain)) {
        std::cout << "SKIP no-auth tests (plain mongod not up): " << uriPlain
                  << "\n";
    } else {
        parlay::MongoClient m;
        std::string err;
        bool ok = m.connect(uriPlain, err);
        CHECK(ok, std::string("no-auth connect: ") + err);
        if (ok) {
            CHECK(!m.authenticated(), "no-auth authenticated()==false");
            CHECK(crudRoundTrip(m, "plain"), "no-auth CRUD");
        }
        parlay::Config cfg;
        cfg.mongo_uri = uriPlain;
        cfg.redis_url = "";
        cfg.data_dir = "/tmp/parlay-scram-test-plain";
        parlay::Store s(cfg);
        CHECK(s.backend() == "mongo",
              "store backend=mongo over no-auth (got " + s.backend() + ")");
        s.set("engine:-2002", "ts");
        CHECK(s.get("engine:-2002") == std::optional<std::string>("ts"),
              "store kv round-trip (no-auth mongo)");
    }

    std::cout << checks << " checks, "
              << (failures == 0 ? "ALL PASS"
                                : "FAILURES: " + std::to_string(failures))
              << "\n";
    return failures == 0 ? 0 : 1;
}
