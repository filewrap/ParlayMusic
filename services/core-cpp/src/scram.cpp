// scram.cpp — self-contained SHA-1 / SHA-256 / HMAC / PBKDF2 / base64 and
// the SCRAM client side (RFC 5802, RFC 7677). No external crypto library.
#include "scram.hpp"

#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace parlay::scram {
namespace {

// --- SHA-256 (FIPS 180-4) -------------------------------------------------
constexpr std::array<uint32_t, 64> kSha256K = {
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
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

std::string sha256Impl(const std::string& data) {
    std::array<uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                 0xa54ff53a, 0x510e527f, 0x9b05688c,
                                 0x1f83d9ab, 0x5be0cd19};
    std::string msg = data;
    uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8;
    msg.push_back('\x80');
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; --i)
        msg.push_back(static_cast<char>((bit_len >> (i * 8)) & 0xff));
    for (size_t off = 0; off < msg.size(); off += 64) {
        std::array<uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i) {
            const auto* p =
                reinterpret_cast<const unsigned char*>(msg.data() + off +
                                                       i * 4);
            w[i] = (static_cast<uint32_t>(p[0]) << 24) |
                   (static_cast<uint32_t>(p[1]) << 16) |
                   (static_cast<uint32_t>(p[2]) << 8) |
                   static_cast<uint32_t>(p[3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^
                          (w[i - 15] >> 3);
            uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^
                          (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5],
                 g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = hh + s1 + ch + kSha256K[i] + w[i];
            uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    std::string out;
    for (uint32_t v : h)
        for (int i = 3; i >= 0; --i)
            out.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
    return out;
}

// --- SHA-1 (FIPS 180-4) ----------------------------------------------------
uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

std::string sha1Impl(const std::string& data) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
             h3 = 0x10325476, h4 = 0xC3D2E1F0;
    std::string msg = data;
    uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8;
    msg.push_back('\x80');
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; --i)
        msg.push_back(static_cast<char>((bit_len >> (i * 8)) & 0xff));
    for (size_t off = 0; off < msg.size(); off += 64) {
        std::array<uint32_t, 80> w{};
        for (int i = 0; i < 16; ++i) {
            const auto* p =
                reinterpret_cast<const unsigned char*>(msg.data() + off +
                                                       i * 4);
            w[i] = (static_cast<uint32_t>(p[0]) << 24) |
                   (static_cast<uint32_t>(p[1]) << 16) |
                   (static_cast<uint32_t>(p[2]) << 8) |
                   static_cast<uint32_t>(p[3]);
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t tmp = rotl32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl32(b, 30);
            b = a;
            a = tmp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }
    std::string out;
    for (uint32_t v : {h0, h1, h2, h3, h4})
        for (int i = 3; i >= 0; --i)
            out.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
    return out;
}

// --- MD5 (RFC 1321) ------------------------------------------------------
// Only used for MongoDB's SCRAM-SHA-1 password derivation (below); the
// SCRAM hashes themselves are SHA-1 / SHA-256.
std::string md5Impl(const std::string& data) {
    auto lrot = [](uint32_t x, int c) { return (x << c) | (x >> (32 - c)); };
    static constexpr std::array<uint32_t, 64> kK = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf,
        0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
        0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e,
        0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6,
        0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
        0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039,
        0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
        0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
        0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static constexpr std::array<int, 64> kS = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    std::string msg = data;
    uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8;
    msg.push_back('\x80');
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 0; i < 8; ++i)
        msg.push_back(static_cast<char>((bit_len >> (i * 8)) & 0xff));
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe,
             d0 = 0x10325476;
    for (size_t off = 0; off < msg.size(); off += 64) {
        std::array<uint32_t, 16> m{};
        for (int i = 0; i < 16; ++i) {
            const auto* p =
                reinterpret_cast<const unsigned char*>(msg.data() + off +
                                                       i * 4);
            m[i] = static_cast<uint32_t>(p[0]) |
                   (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) |
                   (static_cast<uint32_t>(p[3]) << 24);
        }
        uint32_t a = a0, b = b0, c = c0, d = d0;
        for (int i = 0; i < 64; ++i) {
            uint32_t f = 0;
            int g = 0;
            if (i < 16) {
                f = (b & c) | ((~b) & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | ((~d) & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | (~d));
                g = (7 * i) % 16;
            }
            f = f + a + kK[i] + m[g];
            a = d;
            d = c;
            c = b;
            b = b + lrot(f, kS[i]);
        }
        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    }
    std::string out;
    for (uint32_t v : {a0, b0, c0, d0})
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
    return out;
}

std::string hexEncode(const std::string& raw) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : raw) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0xf]);
    }
    return out;
}
std::string hmacImpl(const std::string& key, const std::string& msg,
                     std::string (*hashFn)(const std::string&),
                     size_t block_size, size_t hash_len) {
    std::string k = key;
    if (k.size() > block_size) k = hashFn(k);
    k.resize(block_size, '\0');
    std::string ipad(block_size, '\x36'), opad(block_size, '\x5c');
    for (size_t i = 0; i < block_size; ++i) {
        ipad[i] = static_cast<char>(ipad[i] ^ k[i]);
        opad[i] = static_cast<char>(opad[i] ^ k[i]);
    }
    (void)hash_len;
    return hashFn(opad + hashFn(ipad + msg));
}

std::string pbkdf2Impl(const std::string& password, const std::string& salt,
                       int iterations, std::size_t dk_len,
                       std::string (*hmacFn)(const std::string&,
                                              const std::string&),
                       size_t hash_len) {
    std::string out;
    uint32_t block = 1;
    while (out.size() < dk_len) {
        std::string salt_block = salt;
        for (int i = 3; i >= 0; --i)
            salt_block.push_back(static_cast<char>((block >> (i * 8)) & 0xff));
        std::string u = hmacFn(password, salt_block);
        std::string t = u;
        for (int i = 1; i < iterations; ++i) {
            u = hmacFn(password, u);
            for (size_t j = 0; j < hash_len; ++j) t[j] ^= u[j];
        }
        out += t;
        ++block;
    }
    out.resize(dk_len);
    return out;
}

// --- SCRAM message helpers --------------------------------------------------
// Split "k=v,k=v,..." on commas that are not inside a value (SCRAM values
// never contain ',' or '=' unescaped here; '=' in a value is base64
// padding, which only ever appears at the end of a value).
bool parseAttrs(const std::string& msg,
                std::map<std::string, std::string>& out) {
    size_t pos = 0;
    while (pos < msg.size()) {
        size_t eq = msg.find('=', pos);
        if (eq == std::string::npos || eq == pos) return false;
        std::string key = msg.substr(pos, eq - pos);
        size_t comma = msg.find(',', eq + 1);
        std::string val = msg.substr(
            eq + 1, comma == std::string::npos ? std::string::npos
                                               : comma - eq - 1);
        out[key] = val;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return true;
}

std::string xorBytes(const std::string& a, const std::string& b) {
    std::string out(a.size(), '\0');
    for (size_t i = 0; i < a.size(); ++i)
        out[i] = static_cast<char>(a[i] ^ b[i]);
    return out;
}

}  // namespace

std::string sha1(const std::string& data) { return sha1Impl(data); }
std::string sha256(const std::string& data) { return sha256Impl(data); }
std::string md5(const std::string& data) { return md5Impl(data); }

std::string mongoSha1Password(const std::string& username,
                              const std::string& password) {
    // MongoDB derives SCRAM-SHA-1 credentials from the legacy MONGODB-CR
    // digest, not the raw password (verified live against mongod 8.0.4:
    // raw password fails, this digest authenticates). SCRAM-SHA-256
    // uses the raw password. This is a MongoDB quirk, not part of RFC
    // 5802 — the scram::Client itself stays pure-RFC and takes the
    // effective password as given.
    return hexEncode(md5Impl(username + ":mongo:" + password));
}

std::string hmacSha1(const std::string& key, const std::string& msg) {
    return hmacImpl(key, msg, sha1Impl, 64, 20);
}
std::string hmacSha256(const std::string& key, const std::string& msg) {
    return hmacImpl(key, msg, sha256Impl, 64, 32);
}

std::string pbkdf2Sha1(const std::string& password, const std::string& salt,
                       int iterations, std::size_t dk_len) {
    return pbkdf2Impl(password, salt, iterations, dk_len, hmacSha1, 20);
}
std::string pbkdf2Sha256(const std::string& password, const std::string& salt,
                         int iterations, std::size_t dk_len) {
    return pbkdf2Impl(password, salt, iterations, dk_len, hmacSha256, 32);
}

std::string base64Encode(const std::string& data) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t n = static_cast<unsigned char>(data[i]) << 16;
        size_t rem = data.size() - i;
        if (rem > 1) n |= static_cast<unsigned char>(data[i + 1]) << 8;
        if (rem > 2) n |= static_cast<unsigned char>(data[i + 2]);
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out.push_back(rem > 1 ? kAlphabet[(n >> 6) & 63] : '=');
        out.push_back(rem > 2 ? kAlphabet[n & 63] : '=');
    }
    return out;
}

bool base64Decode(const std::string& in, std::string& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    if (in.size() % 4 != 0) return false;
    out.clear();
    for (size_t i = 0; i < in.size(); i += 4) {
        int pad = 0;
        if (in[i + 3] == '=') ++pad;
        if (in[i + 2] == '=') ++pad;
        if (pad > 0 && i + 4 != in.size()) return false;  // '=' only at end
        int v[4];
        for (int j = 0; j < 4; ++j) {
            if (in[i + j] == '=') {
                v[j] = 0;
            } else {
                v[j] = val(in[i + j]);
                if (v[j] < 0) return false;
            }
        }
        uint32_t n = (static_cast<uint32_t>(v[0]) << 18) |
                     (static_cast<uint32_t>(v[1]) << 12) |
                     (static_cast<uint32_t>(v[2]) << 6) |
                     static_cast<uint32_t>(v[3]);
        out.push_back(static_cast<char>((n >> 16) & 0xff));
        if (pad < 2) out.push_back(static_cast<char>((n >> 8) & 0xff));
        if (pad < 1) out.push_back(static_cast<char>(n & 0xff));
    }
    return true;
}

std::string randomNonceBase64(std::size_t n_bytes) {
    std::string raw(n_bytes, '\0');
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    bool ok = false;
    if (urandom) {
        urandom.read(raw.data(), static_cast<std::streamsize>(n_bytes));
        ok = urandom.gcount() == static_cast<std::streamsize>(n_bytes);
    }
    if (!ok) {
        // Fallback: hash(time + pid + counter). Never expected on Linux,
        // but a nonce must never repeat across attempts.
        static uint64_t counter = 0;
        std::ostringstream ss;
        ss << std::chrono::steady_clock::now().time_since_epoch().count()
           << getpid() << ++counter;
        std::string seed = sha256(ss.str());
        for (size_t i = 0; i < n_bytes; ++i)
            raw[i] = static_cast<char>(seed[i % seed.size()] ^
                                       static_cast<char>(i * 31));
    }
    return base64Encode(raw);
}

const char* mechanismName(Mechanism m) {
    return m == Mechanism::kSha256 ? "SCRAM-SHA-256" : "SCRAM-SHA-1";
}

bool parseMechanism(const std::string& name, Mechanism& out) {
    if (name == "SCRAM-SHA-256") {
        out = Mechanism::kSha256;
        return true;
    }
    if (name == "SCRAM-SHA-1") {
        out = Mechanism::kSha1;
        return true;
    }
    return false;
}

// --- Client ------------------------------------------------------------------
Client::Client(Mechanism mechanism, std::string username,
               std::string password, std::string nonce_b64)
    : mechanism_(mechanism),
      username_(std::move(username)),
      password_(std::move(password)) {
    // SCRAM username escaping: '=' -> "=3D", ',' -> "=2C" (RFC 5802 §5.1).
    // (Passwords are not escaped; they go through PBKDF2, not the message.)
    std::string escaped;
    for (char c : username_) {
        if (c == '=') escaped += "=3D";
        else if (c == ',') escaped += "=2C";
        else escaped += c;
    }
    std::string nonce = nonce_b64.empty() ? randomNonceBase64() : nonce_b64;
    client_first_bare_ = "n=" + escaped + ",r=" + nonce;
}

std::string Client::clientFirst() const {
    return "n,," + client_first_bare_;
}

bool Client::serverFirst(const std::string& server_first,
                         std::string& error_out) {
    error_out.clear();
    std::map<std::string, std::string> attrs;
    if (!parseAttrs(server_first, attrs) || !attrs.count("r") ||
        !attrs.count("s") || !attrs.count("i")) {
        error_out = "malformed SCRAM server-first-message: " + server_first;
        return false;
    }
    full_nonce_ = attrs["r"];
    // The server nonce must extend our client nonce (RFC 5802 §5.1).
    std::string client_nonce =
        client_first_bare_.substr(client_first_bare_.find("r=") + 2);
    if (full_nonce_.rfind(client_nonce, 0) != 0) {
        error_out = "SCRAM server nonce does not start with client nonce";
        return false;
    }
    std::string salt;
    if (!base64Decode(attrs["s"], salt)) {
        error_out = "SCRAM server-first-message has invalid base64 salt";
        return false;
    }
    int iterations = 0;
    try {
        size_t used = 0;
        iterations = std::stoi(attrs["i"], &used);
        if (used != attrs["i"].size() || iterations < 1) throw 0;
    } catch (...) {
        error_out = "SCRAM server-first-message has invalid iteration count";
        return false;
    }
    // Cap iterations at a sane bound so a hostile server cannot make us
    // PBKDF2-loop forever (MongoDB defaults to 10,000).
    if (iterations > 1000000) {
        error_out = "SCRAM iteration count unreasonably large";
        return false;
    }
    const bool use256 = mechanism_ == Mechanism::kSha256;
    salted_password_ =
        use256 ? pbkdf2Sha256(password_, salt, iterations, 32)
               : pbkdf2Sha1(password_, salt, iterations, 20);
    std::string client_final_wo_proof = "c=biws,r=" + full_nonce_;
    auth_message_ = client_first_bare_ + "," + server_first + "," +
                    client_final_wo_proof;
    have_server_first_ = true;
    return true;
}

std::string Client::clientFinal() const {
    if (!have_server_first_) return "";
    const bool use256 = mechanism_ == Mechanism::kSha256;
    std::string client_key = use256
        ? hmacSha256(salted_password_, "Client Key")
        : hmacSha1(salted_password_, "Client Key");
    std::string stored_key =
        use256 ? sha256(client_key) : sha1(client_key);
    std::string client_sig = use256 ? hmacSha256(stored_key, auth_message_)
                                    : hmacSha1(stored_key, auth_message_);
    std::string proof = xorBytes(client_key, client_sig);
    return "c=biws,r=" + full_nonce_ + ",p=" + base64Encode(proof);
}

bool Client::serverFinal(const std::string& server_final,
                         std::string& error_out) {
    error_out.clear();
    if (!have_server_first_) {
        error_out = "SCRAM server-final received before server-first";
        return false;
    }
    std::map<std::string, std::string> attrs;
    if (!parseAttrs(server_final, attrs)) {
        error_out = "malformed SCRAM server-final-message: " + server_final;
        return false;
    }
    if (attrs.count("e")) {
        error_out = "SCRAM authentication failed (server error): " +
                    attrs["e"];
        return false;
    }
    if (!attrs.count("v")) {
        error_out = "SCRAM server-final-message has no verifier (v=)";
        return false;
    }
    const bool use256 = mechanism_ == Mechanism::kSha256;
    std::string server_key = use256
        ? hmacSha256(salted_password_, "Server Key")
        : hmacSha1(salted_password_, "Server Key");
    std::string expected = use256
        ? base64Encode(hmacSha256(server_key, auth_message_))
        : base64Encode(hmacSha1(server_key, auth_message_));
    if (attrs["v"] != expected) {
        error_out = "SCRAM server signature mismatch (possible MITM)";
        return false;
    }
    return true;
}

}  // namespace parlay::scram
