// scram.cpp — self-contained SHA-1 / SHA-256 / HMAC / PBKDF2 / base64.
// No external crypto library. The SCRAM client side (RFC 5802, RFC 7677)
// that used to live here was deleted in Phase 4 (Q1) with the database
// client it authenticated; the hash/HMAC/PBKDF2/base64 primitives stay:
// sha256 digests track ids (ui_texts.hpp) and base64 encodes TDLib
// `bytes` fields (dispatcher.cpp).
#include "scram.hpp"

#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
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

}  // namespace

std::string sha1(const std::string& data) { return sha1Impl(data); }
std::string sha256(const std::string& data) { return sha256Impl(data); }

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

}  // namespace parlay::scram
