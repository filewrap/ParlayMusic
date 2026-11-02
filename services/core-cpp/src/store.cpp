// store.cpp
#include "store.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>

#include "json.hpp"

namespace parlay {
namespace {

int connectTcp(const std::string& host, int port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0)
        return -1;
    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        timeval tv{};
        tv.tv_sec = 5;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

bool writeAllFd(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(fd, data.data() + off, data.size() - off, 0);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

void putI32(std::string& out, int32_t v) {
    out.append(reinterpret_cast<const char*>(&v), 4);  // x86_64: little-endian
}
void putI64(std::string& out, int64_t v) {
    out.append(reinterpret_cast<const char*>(&v), 8);
}
void putCStr(std::string& out, const std::string& s) {
    out += s;
    out.push_back('\0');
}

}  // namespace

// --- Redis -----------------------------------------------------------------
RedisClient::~RedisClient() { close(); }

void RedisClient::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool RedisClient::connect(const std::string& url, std::string& error_out) {
    // redis://[:pass@]host[:port][/db]
    std::string rest = url;
    const std::string prefix = "redis://";
    if (rest.rfind(prefix, 0) == 0) rest = rest.substr(prefix.size());
    std::string password;
    if (auto at = rest.find('@'); at != std::string::npos) {
        std::string userinfo = rest.substr(0, at);
        rest = rest.substr(at + 1);
        if (auto colon = userinfo.find(':'); colon != std::string::npos)
            password = userinfo.substr(colon + 1);
        else
            password = userinfo;
    }
    std::string host = "127.0.0.1";
    int port = 6379;
    int db = 0;
    if (auto slash = rest.find('/'); slash != std::string::npos) {
        try {
            db = std::stoi(rest.substr(slash + 1));
        } catch (...) {
        }
        rest = rest.substr(0, slash);
    }
    if (auto colon = rest.rfind(':'); colon != std::string::npos) {
        host = rest.substr(0, colon);
        try {
            port = std::stoi(rest.substr(colon + 1));
        } catch (...) {
        }
    } else if (!rest.empty()) {
        host = rest;
    }
    fd_ = connectTcp(host, port);
    if (fd_ < 0) {
        error_out = "cannot connect to redis at " + host + ":" +
                    std::to_string(port);
        return false;
    }
    if (!password.empty()) {
        Reply r;
        if (!command({"AUTH", password}, r, error_out) || r.kind == '-') {
            error_out = "redis AUTH failed: " + r.str;
            close();
            return false;
        }
    }
    if (db != 0) {
        Reply r;
        if (!command({"SELECT", std::to_string(db)}, r, error_out) ||
            r.kind == '-') {
            error_out = "redis SELECT failed: " + r.str;
            close();
            return false;
        }
    }
    return ping();
}

bool RedisClient::ping() {
    Reply r;
    std::string err;
    if (!command({"PING"}, r, err)) return false;
    return r.kind == '+' && r.str == "PONG";
}

bool RedisClient::writeAll(const std::string& data) {
    return writeAllFd(fd_, data);
}

bool RedisClient::readLine(std::string& line_out) {
    while (true) {
        if (auto pos = read_buf_.find('\n'); pos != std::string::npos) {
            line_out = read_buf_.substr(0, pos);
            if (!line_out.empty() && line_out.back() == '\r')
                line_out.pop_back();
            read_buf_.erase(0, pos + 1);
            return true;
        }
        char tmp[4096];
        ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        read_buf_.append(tmp, static_cast<size_t>(n));
    }
}

bool RedisClient::readExact(char* buf, size_t n) {
    size_t got = 0;
    if (!read_buf_.empty()) {
        size_t take = std::min(n, read_buf_.size());
        std::memcpy(buf, read_buf_.data(), take);
        read_buf_.erase(0, take);
        got = take;
    }
    while (got < n) {
        ssize_t r = ::recv(fd_, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

bool RedisClient::command(const std::vector<std::string>& argv, Reply& out,
                          std::string& error_out) {
    if (fd_ < 0) {
        error_out = "redis not connected";
        return false;
    }
    std::string wire = "*" + std::to_string(argv.size()) + "\r\n";
    for (const auto& a : argv)
        wire += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
    if (!writeAll(wire)) {
        error_out = "redis write failed";
        return false;
    }
    std::string line;
    if (!readLine(line)) {
        error_out = "redis read failed";
        return false;
    }
    if (line.empty()) {
        error_out = "redis empty reply";
        return false;
    }
    out.kind = line[0];
    std::string payload = line.substr(1);
    switch (line[0]) {
        case '+':
            out.str = payload;
            return true;
        case '-':
            out.str = payload;
            error_out = payload;
            return true;
        case ':':
            try {
                out.num = std::stoll(payload);
            } catch (...) {
                out.num = 0;
            }
            return true;
        case '$': {
            int len = 0;
            try {
                len = std::stoi(payload);
            } catch (...) {
                error_out = "bad bulk length";
                return false;
            }
            if (len < 0) {
                out.isNil = true;
                return true;
            }
            std::string buf(static_cast<size_t>(len), '\0');
            if (!readExact(buf.data(), buf.size())) {
                error_out = "redis bulk read failed";
                return false;
            }
            char crlf[2];
            if (!readExact(crlf, 2)) {
                error_out = "redis bulk trailer read failed";
                return false;
            }
            out.str = buf;
            return true;
        }
        case '*': {
            int count = 0;
            try {
                count = std::stoi(payload);
            } catch (...) {
                error_out = "bad array length";
                return false;
            }
            if (count < 0) {
                out.isNil = true;
                return true;
            }
            out.arr.reserve(static_cast<size_t>(count));
            for (int i = 0; i < count; ++i) {
                Reply el;
                // Parse one nested reply inline (no extra command sent).
                std::string l2;
                if (!readLine(l2)) {
                    error_out = "redis array read failed";
                    return false;
                }
                el.kind = l2.empty() ? 0 : l2[0];
                std::string p2 = l2.size() > 1 ? l2.substr(1) : "";
                if (el.kind == '+' || el.kind == '-') {
                    el.str = p2;
                } else if (el.kind == ':') {
                    try {
                        el.num = std::stoll(p2);
                    } catch (...) {
                    }
                } else if (el.kind == '$') {
                    int blen = 0;
                    try {
                        blen = std::stoi(p2);
                    } catch (...) {
                    }
                    if (blen < 0) {
                        el.isNil = true;
                    } else {
                        std::string b(static_cast<size_t>(blen), '\0');
                        if (!readExact(b.data(), b.size())) {
                            error_out = "redis array bulk read failed";
                            return false;
                        }
                        char crlf[2];
                        readExact(crlf, 2);
                        el.str = b;
                    }
                } else {
                    error_out = "unsupported nested RESP type";
                    return false;
                }
                out.arr.push_back(std::move(el));
            }
            return true;
        }
        default:
            error_out = std::string("unsupported RESP type: ") + line[0];
            return false;
    }
}

bool RedisClient::get(const std::string& key, std::string& value_out) {
    Reply r;
    std::string err;
    if (!command({"GET", key}, r, err)) return false;
    if (r.isNil) return false;
    value_out = r.str;
    return true;
}

bool RedisClient::set(const std::string& key, const std::string& value) {
    Reply r;
    std::string err;
    return command({"SET", key, value}, r, err) && r.kind != '-';
}

bool RedisClient::setEx(const std::string& key, const std::string& value,
                        int ttl_sec) {
    Reply r;
    std::string err;
    return command({"SET", key, value, "EX", std::to_string(ttl_sec)}, r,
                   err) &&
           r.kind != '-';
}

bool RedisClient::del(const std::string& key) {
    Reply r;
    std::string err;
    return command({"DEL", key}, r, err) && r.kind != '-';
}

bool RedisClient::sadd(const std::string& key, const std::string& member) {
    Reply r;
    std::string err;
    return command({"SADD", key, member}, r, err) && r.kind != '-';
}

bool RedisClient::srem(const std::string& key, const std::string& member) {
    Reply r;
    std::string err;
    return command({"SREM", key, member}, r, err) && r.kind != '-';
}

std::vector<std::string> RedisClient::smembers(const std::string& key) {
    Reply r;
    std::string err;
    std::vector<std::string> out;
    if (!command({"SMEMBERS", key}, r, err) || r.kind == '-') return out;
    for (const auto& el : r.arr)
        if (!el.isNil) out.push_back(el.str);
    return out;
}

bool RedisClient::hset(const std::string& key, const std::string& field,
                       const std::string& value) {
    Reply r;
    std::string err;
    return command({"HSET", key, field, value}, r, err) && r.kind != '-';
}

bool RedisClient::hdel(const std::string& key, const std::string& field) {
    Reply r;
    std::string err;
    return command({"HDEL", key, field}, r, err) && r.kind != '-';
}

bool RedisClient::hget(const std::string& key, const std::string& field,
                       std::string& value_out) {
    Reply r;
    std::string err;
    if (!command({"HGET", key, field}, r, err)) return false;
    if (r.isNil) return false;
    value_out = r.str;
    return true;
}

std::map<std::string, std::string> RedisClient::hgetall(
    const std::string& key) {
    Reply r;
    std::string err;
    std::map<std::string, std::string> out;
    if (!command({"HGETALL", key}, r, err) || r.kind == '-') return out;
    for (size_t i = 0; i + 1 < r.arr.size(); i += 2)
        out[r.arr[i].str] = r.arr[i + 1].str;
    return out;
}

std::vector<std::string> RedisClient::keys(const std::string& pattern) {
    Reply r;
    std::string err;
    std::vector<std::string> out;
    if (!command({"KEYS", pattern}, r, err) || r.kind == '-') return out;
    for (const auto& el : r.arr)
        if (!el.isNil) out.push_back(el.str);
    return out;
}

// --- BSON ------------------------------------------------------------------
namespace bson {
namespace {
std::string wrapDoc(std::string elements) {
    std::string out;
    putI32(out, static_cast<int32_t>(elements.size() + 5));
    out += elements;
    out.push_back('\0');
    return out;
}
}  // namespace

std::string doc(std::vector<std::pair<std::string, std::string>> fields) {
    std::string elements;
    for (auto& f : fields) elements += f.second;  // fields carry key already
    (void)fields;
    return wrapDoc(elements);
}

std::string elString(const std::string& key, const std::string& value) {
    std::string out;
    out.push_back('\x02');
    putCStr(out, key);
    putI32(out, static_cast<int32_t>(value.size() + 1));
    out += value;
    out.push_back('\0');
    return out;
}
std::string elInt32(const std::string& key, int32_t value) {
    std::string out;
    out.push_back('\x10');
    putCStr(out, key);
    putI32(out, value);
    return out;
}
std::string elInt64(const std::string& key, int64_t value) {
    std::string out;
    out.push_back('\x12');
    putCStr(out, key);
    putI64(out, value);
    return out;
}
std::string elDouble(const std::string& key, double value) {
    std::string out;
    out.push_back('\x01');
    putCStr(out, key);
    out.append(reinterpret_cast<const char*>(&value), 8);
    return out;
}
std::string elBool(const std::string& key, bool value) {
    std::string out;
    out.push_back('\x08');
    putCStr(out, key);
    out.push_back(value ? '\x01' : '\x00');
    return out;
}
std::string elDoc(const std::string& key, const std::string& doc_bson) {
    std::string out;
    out.push_back('\x03');
    putCStr(out, key);
    out += doc_bson;
    return out;
}
std::string elArray(const std::string& key,
                    const std::vector<std::string>& string_items) {
    std::string elements;
    for (size_t i = 0; i < string_items.size(); ++i)
        elements += elString(std::to_string(i), string_items[i]);
    std::string out;
    out.push_back('\x04');
    putCStr(out, key);
    out += wrapDoc(elements);
    return out;
}
std::string elArrayOfDocs(const std::string& key,
                          const std::vector<std::string>& docs) {
    std::string elements;
    for (size_t i = 0; i < docs.size(); ++i)
        elements += elDoc(std::to_string(i), docs[i]);
    std::string out;
    out.push_back('\x04');
    putCStr(out, key);
    out += wrapDoc(elements);
    return out;
}
std::string elRegex(const std::string& key, const std::string& pattern) {
    std::string out;
    out.push_back('\x0b');
    putCStr(out, key);
    putCStr(out, pattern);
    putCStr(out, "");
    return out;
}

namespace {
// Walk a BSON document; invoke fn(type, key, value_offset) per element.
// Returns false when the document is malformed.
bool walk(const std::string& doc,
          const std::function<bool(char, const std::string&, size_t)>& fn) {
    if (doc.size() < 5) return false;
    size_t pos = 4;
    while (pos < doc.size()) {
        unsigned char type = static_cast<unsigned char>(doc[pos]);
        if (type == 0) return true;
        size_t key_end = doc.find('\0', pos + 1);
        if (key_end == std::string::npos) return false;
        std::string key = doc.substr(pos + 1, key_end - pos - 1);
        size_t vpos = key_end + 1;
        size_t next = 0;
        switch (type) {
            case 0x01:
                next = vpos + 8;
                break;
            case 0x02:
            case 0x0d:
            case 0x0e: {
                if (vpos + 4 > doc.size()) return false;
                int32_t len;
                std::memcpy(&len, doc.data() + vpos, 4);
                next = vpos + 4 + static_cast<size_t>(len);
                break;
            }
            case 0x03:
            case 0x04: {
                if (vpos + 4 > doc.size()) return false;
                int32_t len;
                std::memcpy(&len, doc.data() + vpos, 4);
                next = vpos + static_cast<size_t>(len);
                break;
            }
            case 0x05: {
                if (vpos + 5 > doc.size()) return false;
                int32_t len;
                std::memcpy(&len, doc.data() + vpos, 4);
                next = vpos + 5 + static_cast<size_t>(len);
                break;
            }
            case 0x06:
            case 0x0a:
                next = vpos;
                break;
            case 0x07:
                next = vpos + 12;
                break;
            case 0x08:
                next = vpos + 1;
                break;
            case 0x09:
            case 0x11:
            case 0x12:
                next = vpos + 8;
                break;
            case 0x0b: {
                size_t a = doc.find('\0', vpos);
                if (a == std::string::npos) return false;
                size_t b = doc.find('\0', a + 1);
                if (b == std::string::npos) return false;
                next = b + 1;
                break;
            }
            case 0x0f: {
                if (vpos + 4 > doc.size()) return false;
                int32_t len;
                std::memcpy(&len, doc.data() + vpos, 4);
                next = vpos + static_cast<size_t>(len);
                break;
            }
            case 0x10:
                next = vpos + 4;
                break;
            case 0x13:
                next = vpos + 4;
                break;
            default:
                return false;
        }
        if (next > doc.size()) return false;
        if (!fn(static_cast<char>(type), key, vpos)) return false;
        pos = next;
    }
    return false;
}

bool readStringAt(const std::string& doc, size_t vpos, std::string& out) {
    if (vpos + 4 > doc.size()) return false;
    int32_t len;
    std::memcpy(&len, doc.data() + vpos, 4);
    if (len <= 0 || vpos + 4 + static_cast<size_t>(len) > doc.size())
        return false;
    out = doc.substr(vpos + 4, static_cast<size_t>(len) - 1);
    return true;
}
}  // namespace

bool getString(const std::string& doc, const std::string& key,
               std::string& out) {
    bool found = false;
    walk(doc, [&](char type, const std::string& k, size_t vpos) {
        if (k == key && type == 0x02) {
            found = readStringAt(doc, vpos, out);
            return false;
        }
        return true;
    });
    return found;
}

bool getDouble(const std::string& doc, const std::string& key, double& out) {
    bool found = false;
    walk(doc, [&](char type, const std::string& k, size_t vpos) {
        if (k == key && (type == 0x01 || type == 0x10 || type == 0x12)) {
            if (type == 0x01) {
                std::memcpy(&out, doc.data() + vpos, 8);
            } else if (type == 0x10) {
                int32_t v;
                std::memcpy(&v, doc.data() + vpos, 4);
                out = v;
            } else {
                int64_t v;
                std::memcpy(&v, doc.data() + vpos, 8);
                out = static_cast<double>(v);
            }
            found = true;
            return false;
        }
        return true;
    });
    return found;
}

bool getInt32(const std::string& doc, const std::string& key, int32_t& out) {
    bool found = false;
    walk(doc, [&](char type, const std::string& k, size_t vpos) {
        if (k == key && type == 0x10) {
            std::memcpy(&out, doc.data() + vpos, 4);
            found = true;
            return false;
        }
        return true;
    });
    return found;
}

bool getFirstArrayDoc(const std::string& doc, const std::string& key,
                      std::string& out) {
    bool found = false;
    walk(doc, [&](char type, const std::string& k, size_t vpos) {
        if (k != key || type != 0x04) return true;
        // Array is a document with keys "0", "1", ...; take element "0".
        std::string arr = doc.substr(vpos);
        int32_t len = 0;
        std::memcpy(&len, arr.data(), 4);
        if (len > 0) arr = arr.substr(0, static_cast<size_t>(len));
        walk(arr, [&](char t2, const std::string& k2, size_t v2) {
            if (k2 == "0" && t2 == 0x03) {
                int32_t dlen = 0;
                std::memcpy(&dlen, arr.data() + v2, 4);
                out = arr.substr(v2, static_cast<size_t>(dlen));
                found = true;
                return false;
            }
            return true;
        });
        return false;
    });
    return found;
}

std::vector<std::string> getStringArray(const std::string& doc,
                                        const std::string& key) {
    std::vector<std::string> out;
    walk(doc, [&](char type, const std::string& k, size_t vpos) {
        if (k != key || type != 0x04) return true;
        std::string arr = doc.substr(vpos);
        int32_t len = 0;
        std::memcpy(&len, arr.data(), 4);
        if (len > 0) arr = arr.substr(0, static_cast<size_t>(len));
        walk(arr, [&](char t2, const std::string&, size_t v2) {
            if (t2 == 0x02) {
                std::string s;
                if (readStringAt(arr, v2, s)) out.push_back(s);
            }
            return true;
        });
        return false;
    });
    return out;
}

}  // namespace bson

// --- MongoDB wire protocol ---------------------------------------------------
MongoClient::~MongoClient() { close(); }

void MongoClient::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool MongoClient::connect(const std::string& uri, std::string& error_out) {
    std::string rest = uri;
    const std::string prefix = "mongodb://";
    if (rest.rfind(prefix, 0) == 0) rest = rest.substr(prefix.size());
    if (auto at = rest.find('@'); at != std::string::npos) {
        error_out =
            "mongodb URIs with credentials are not supported by the minimal "
            "wire-protocol client (needs SCRAM auth); use an unauthenticated "
            "local URI";
        return false;
    }
    if (auto slash = rest.find('/'); slash != std::string::npos) {
        std::string dbpart = rest.substr(slash + 1);
        if (auto q = dbpart.find('?'); q != std::string::npos)
            dbpart = dbpart.substr(0, q);
        if (!dbpart.empty()) db_ = dbpart;
        rest = rest.substr(0, slash);
    }
    host_ = "127.0.0.1";
    port_ = 27017;
    if (auto colon = rest.rfind(':'); colon != std::string::npos) {
        host_ = rest.substr(0, colon);
        try {
            port_ = std::stoi(rest.substr(colon + 1));
        } catch (...) {
        }
    } else if (!rest.empty()) {
        host_ = rest;
    }
    fd_ = connectTcp(host_, port_);
    if (fd_ < 0) {
        error_out = "cannot connect to mongodb at " + host_ + ":" +
                    std::to_string(port_);
        return false;
    }
    return ping();
}

bool MongoClient::sendAll(const std::string& data) {
    return writeAllFd(fd_, data);
}

bool MongoClient::recvExact(char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(fd_, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

std::string MongoClient::runCommand(const std::string& body_bson,
                                    std::string& error_out) {
    // OP_MSG: header(16) + flagBits(4) + section kind(1) + body doc.
    std::string msg;
    int32_t total = 16 + 4 + 1 + static_cast<int32_t>(body_bson.size());
    putI32(msg, total);
    putI32(msg, request_id_++);
    putI32(msg, 0);
    putI32(msg, 2013);  // OP_MSG
    putI32(msg, 0);     // flagBits
    msg.push_back('\0');  // section kind 0: body
    msg += body_bson;
    if (!sendAll(msg)) {
        error_out = "mongodb send failed";
        return "";
    }
    char header[16];
    if (!recvExact(header, 16)) {
        error_out = "mongodb recv failed";
        return "";
    }
    int32_t len = 0, op = 0;
    std::memcpy(&len, header, 4);
    std::memcpy(&op, header + 12, 4);
    if (len < 16 || op != 2013) {
        error_out = "mongodb bad reply header";
        return "";
    }
    std::string rest(static_cast<size_t>(len) - 16, '\0');
    if (!recvExact(rest.data(), rest.size())) {
        error_out = "mongodb recv body failed";
        return "";
    }
    // rest: flagBits(4) + sections. First kind-0 section is the body doc.
    if (rest.size() < 5 || rest[4] != 0) {
        error_out = "mongodb unexpected reply sections";
        return "";
    }
    std::string body = rest.substr(5);
    int32_t blen = 0;
    if (body.size() < 4) {
        error_out = "mongodb short body";
        return "";
    }
    std::memcpy(&blen, body.data(), 4);
    if (blen > 0 && static_cast<size_t>(blen) <= body.size())
        body = body.substr(0, static_cast<size_t>(blen));
    double ok = 0;
    if (!bson::getDouble(body, "ok", ok) || ok != 1.0) {
        std::string msg_text;
        bson::getString(body, "errmsg", msg_text);
        error_out = "mongodb command failed: " + msg_text;
        return "";
    }
    return body;
}

bool MongoClient::ping() {
    std::string err;
    std::string body = bson::doc(
        {{ "ping", bson::elInt32("ping", 1) },
         { "$db", bson::elString("$db", "admin") }});
    return !runCommand(body, err).empty();
}

bool MongoClient::findValue(const std::string& key, std::string& value_out) {
    std::string filter = bson::doc({{ "_id", bson::elString("_id", key) }});
    std::string body = bson::doc(
        {{ "find", bson::elString("find", "state") },
         { "filter", bson::elDoc("filter", filter) },
         { "limit", bson::elInt64("limit", 1) },
         { "$db", bson::elString("$db", db_) }});
    std::string err;
    std::string reply = runCommand(body, err);
    if (reply.empty()) return false;
    std::string first;
    // cursor.firstBatch is nested: cursor doc -> firstBatch array -> doc 0.
    // Locate the "cursor" subdocument by byte scan (type 0x03 + "cursor\0").
    std::string cur_doc;
    {
        // Walk reply top-level for key "cursor" (type 0x03).
        // bson::walk is internal; replicate the search using getString on a
        // synthetic path is not possible, so do a byte scan for the key.
        const std::string needle = std::string("cursor\0", 7);
        size_t pos = reply.find(needle);
        if (pos != std::string::npos && pos >= 1 &&
            reply[pos - 1] == 0x03) {
            size_t vpos = pos + needle.size();
            if (vpos + 4 <= reply.size()) {
                int32_t dlen = 0;
                std::memcpy(&dlen, reply.data() + vpos, 4);
                if (dlen > 0 &&
                    vpos + static_cast<size_t>(dlen) <= reply.size())
                    cur_doc = reply.substr(vpos, static_cast<size_t>(dlen));
            }
        }
    }
    if (!cur_doc.empty() && bson::getFirstArrayDoc(cur_doc, "firstBatch", first)) {
        if (bson::getString(first, "v", value_out)) return true;
    }
    return false;
}

bool MongoClient::upsertValue(const std::string& key,
                              const std::string& value) {
    std::string q = bson::doc({{ "_id", bson::elString("_id", key) }});
    std::string set = bson::doc({{ "v", bson::elString("v", value) }});
    std::string upd = bson::doc(
        {{ "q", bson::elDoc("q", q) },
         { "u", bson::elDoc("u", bson::doc(
                    {{ "$set", bson::elDoc("$set", set) }})) },
         { "upsert", bson::elBool("upsert", true) }});
    std::string body = bson::doc(
        {{ "update", bson::elString("update", "state") },
         { "updates", bson::elArrayOfDocs("updates", {upd}) },
         { "$db", bson::elString("$db", db_) }});
    std::string err;
    return !runCommand(body, err).empty();
}

bool MongoClient::deleteKey(const std::string& key) {
    std::string q = bson::doc({{ "_id", bson::elString("_id", key) }});
    std::string del = bson::doc(
        {{ "q", bson::elDoc("q", q) },
         { "limit", bson::elInt32("limit", 1) }});
    std::string body = bson::doc(
        {{ "delete", bson::elString("delete", "state") },
         { "deletes", bson::elArrayOfDocs("deletes", {del}) },
         { "$db", bson::elString("$db", db_) }});
    std::string err;
    return !runCommand(body, err).empty();
}

std::vector<std::string> MongoClient::keysWithPrefix(
    const std::string& prefix) {
    std::string filter = bson::doc(
        {{ "_id", bson::elRegex("_id", "^" + prefix) }});
    std::string body = bson::doc(
        {{ "find", bson::elString("find", "state") },
         { "filter", bson::elDoc("filter", filter) },
         { "$db", bson::elString("$db", db_) }});
    std::string err;
    std::string reply = runCommand(body, err);
    std::vector<std::string> out;
    if (reply.empty()) return out;
    // cursor.firstBatch is an array of docs each with _id.
    const std::string needle = std::string("cursor\0", 7);
    size_t pos = reply.find(needle);
    if (pos == std::string::npos || pos < 1 || reply[pos - 1] != 0x03)
        return out;
    size_t vpos = pos + needle.size();
    if (vpos + 4 > reply.size()) return out;
    int32_t dlen = 0;
    std::memcpy(&dlen, reply.data() + vpos, 4);
    if (dlen <= 0 || vpos + static_cast<size_t>(dlen) > reply.size())
        return out;
    std::string cur_doc = reply.substr(vpos, static_cast<size_t>(dlen));
    // Walk firstBatch array docs and collect _id strings.
    const std::string needle2 = std::string("firstBatch\0", 11);
    size_t p2 = cur_doc.find(needle2);
    if (p2 == std::string::npos || p2 < 1 || cur_doc[p2 - 1] != 0x04)
        return out;
    size_t apos = p2 + needle2.size();
    if (apos + 4 > cur_doc.size()) return out;
    int32_t alen = 0;
    std::memcpy(&alen, cur_doc.data() + apos, 4);
    if (alen <= 0 || apos + static_cast<size_t>(alen) > cur_doc.size())
        return out;
    std::string arr = cur_doc.substr(apos, static_cast<size_t>(alen));
    // Each element is a doc; scan for "_id" string elements inside.
    size_t scan = 4;
    while (scan < arr.size()) {
        const std::string idneedle = std::string("_id\0", 4);
        size_t f = arr.find(idneedle, scan);
        if (f == std::string::npos) break;
        if (f < 1 || arr[f - 1] != 0x02) {
            scan = f + 1;
            continue;
        }
        size_t sp = f + idneedle.size();
        if (sp + 4 > arr.size()) break;
        int32_t slen = 0;
        std::memcpy(&slen, arr.data() + sp, 4);
        if (slen > 0 && sp + 4 + static_cast<size_t>(slen) <= arr.size())
            out.push_back(arr.substr(sp + 4, static_cast<size_t>(slen) - 1));
        scan = sp + 4 + (slen > 0 ? static_cast<size_t>(slen) : 0);
    }
    return out;
}

// --- Store -----------------------------------------------------------------
Store::Store(const Config& cfg) : cfg_(cfg) {
    std::string err;
    if (!cfg.mongo_uri.empty()) {
        auto m = std::make_unique<MongoClient>();
        if (m->connect(cfg.mongo_uri, err)) {
            mongo_ = std::move(m);
            backend_ = "mongo";
        } else {
            std::cerr << "[store] mongo unavailable (" << err
                      << "); trying redis/file\n";
        }
    }
    if (backend_ != "mongo" && !cfg.redis_url.empty()) {
        auto r = std::make_unique<RedisClient>();
        if (r->connect(cfg.redis_url, err)) {
            redis_ = std::move(r);
            backend_ = "redis";
        } else {
            std::cerr << "[store] redis unavailable (" << err
                      << "); trying file\n";
        }
    }
    if (backend_ != "mongo" && backend_ != "redis") {
        backend_ = "file";
        std::string dir = cfg.data_dir.empty() ? "data" : cfg.data_dir;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        file_path_ = dir + "/state.json";
        fileLoad();
    }
    // Redis is also useful as the ephemeral cache even when Mongo is the
    // durable backend.
    if (backend_ == "mongo" && !cfg.redis_url.empty() && !redis_) {
        auto r = std::make_unique<RedisClient>();
        if (r->connect(cfg.redis_url, err)) redis_ = std::move(r);
    }
    std::cout << "[store] backend=" << backend_
              << (mongo_ ? " mongo_db=" + mongo_->dbName() : "")
              << (redis_ ? " redis_cache=yes" : "") << "\n";
}

std::optional<std::string> Store::mongoGet(const std::string& key) {
    if (!mongo_) return std::nullopt;
    std::string v;
    if (mongo_->findValue("kv:" + key, v)) return v;
    return std::nullopt;
}
void Store::mongoSet(const std::string& key, const std::string& value) {
    if (mongo_) mongo_->upsertValue("kv:" + key, value);
}
void Store::mongoDel(const std::string& key) {
    if (mongo_) mongo_->deleteKey("kv:" + key);
}
std::optional<std::string> Store::redisGet(const std::string& key) {
    if (!redis_) return std::nullopt;
    std::string v;
    if (redis_->get("parlay:kv:" + key, v)) return v;
    return std::nullopt;
}
void Store::redisSet(const std::string& key, const std::string& value) {
    if (redis_) redis_->set("parlay:kv:" + key, value);
}
void Store::redisDel(const std::string& key) {
    if (redis_) redis_->del("parlay:kv:" + key);
}

void Store::fileLoad() {
    std::ifstream f(file_path_);
    if (!f) return;
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    if (content.empty()) return;
    Json root;
    try {
        root = Json::parse(content);
    } catch (...) {
        std::cerr << "[store] state.json unparsable; starting empty\n";
        return;
    }
    if (const Json* kv = root.find("kv")) {
        for (const auto& [k, v] : kv->asObject()) file_kv_[k] = v.asString();
    }
    if (const Json* sets = root.find("sets")) {
        for (const auto& [k, arr] : sets->asObject()) {
            for (const auto& el : arr.asArray())
                file_sets_[k].insert(el.asString());
        }
    }
    if (const Json* hashes = root.find("hashes")) {
        for (const auto& [k, h] : hashes->asObject()) {
            for (const auto& [field, v] : h.asObject())
                file_hashes_[k][field] = v.asString();
        }
    }
}

void Store::fileSave() {
    Json kv = Json::objectOf({});
    for (auto& [k, v] : file_kv_) kv.set(k, Json(v));
    Json sets = Json::objectOf({});
    for (auto& [k, s] : file_sets_) {
        Json::Array arr;
        for (auto& m : s) arr.push_back(Json(m));
        sets.set(k, Json(arr));
    }
    Json hashes = Json::objectOf({});
    for (auto& [k, h] : file_hashes_) {
        Json obj = Json::objectOf({});
        for (auto& [f, v] : h) obj.set(f, Json(v));
        hashes.set(k, obj);
    }
    Json root = Json::objectOf(
        {{"kv", kv}, {"sets", sets}, {"hashes", hashes}});
    std::string tmp = file_path_ + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return;
        f << root.dump();
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file_path_, ec);
    if (ec)
        std::cerr << "[store] state.json rename failed: " << ec.message()
                  << "\n";
}

std::optional<std::string> Store::get(const std::string& key) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (backend_ == "mongo") return mongoGet(key);
    if (backend_ == "redis") return redisGet(key);
    auto it = file_kv_.find(key);
    if (it == file_kv_.end()) return std::nullopt;
    return it->second;
}

void Store::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (backend_ == "mongo") {
        mongoSet(key, value);
        return;
    }
    if (backend_ == "redis") {
        redisSet(key, value);
        return;
    }
    file_kv_[key] = value;
    fileSave();
}

void Store::del(const std::string& key) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (backend_ == "mongo") {
        mongoDel(key);
        return;
    }
    if (backend_ == "redis") {
        redisDel(key);
        return;
    }
    file_kv_.erase(key);
    fileSave();
}

std::set<std::string> Store::smembers(const std::string& key) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (backend_ == "mongo") {
        // Sets are stored as one JSON array document under set:<key>.
        auto v = mongoGet("set:" + key);
        if (!v) return {};
        std::set<std::string> out;
        try {
            Json arr = Json::parse(*v);
            for (const auto& el : arr.asArray()) out.insert(el.asString());
        } catch (...) {
        }
        return out;
    }
    if (backend_ == "redis") {
        auto v = redis_->smembers("parlay:set:" + key);
        return std::set<std::string>(v.begin(), v.end());
    }
    auto it = file_sets_.find(key);
    if (it == file_sets_.end()) return {};
    return it->second;
}

void Store::sadd(const std::string& key, const std::string& member) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (backend_ == "mongo") {
        auto cur = smembers(key);
        cur.insert(member);
        Json::Array arr;
        for (auto& m : cur) arr.push_back(Json(m));
        mongoSet("set:" + key, Json(arr).dump());
        return;
    }
    if (backend_ == "redis") {
        redis_->sadd("parlay:set:" + key, member);
        return;
    }
    file_sets_[key].insert(member);
    fileSave();
}

void Store::srem(const std::string& key, const std::string& member) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (backend_ == "mongo") {
        auto cur = smembers(key);
        cur.erase(member);
        Json::Array arr;
        for (auto& m : cur) arr.push_back(Json(m));
        mongoSet("set:" + key, Json(arr).dump());
        return;
    }
    if (backend_ == "redis") {
        redis_->srem("parlay:set:" + key, member);
        return;
    }
    file_sets_[key].erase(member);
    fileSave();
}

bool Store::sismember(const std::string& key, const std::string& member) {
    return smembers(key).count(member) > 0;
}

std::optional<std::string> Store::hget(const std::string& key,
                                       const std::string& field) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (backend_ == "mongo") {
        auto v = mongoGet("hash:" + key);
        if (!v) return std::nullopt;
        try {
            Json obj = Json::parse(*v);
            const Json* f = obj.find(field);
            if (f) return f->asString();
        } catch (...) {
        }
        return std::nullopt;
    }
    if (backend_ == "redis") {
        std::string v;
        if (redis_->hget("parlay:hash:" + key, field, v)) return v;
        return std::nullopt;
    }
    auto it = file_hashes_.find(key);
    if (it == file_hashes_.end()) return std::nullopt;
    auto fit = it->second.find(field);
    if (fit == it->second.end()) return std::nullopt;
    return fit->second;
}

void Store::hset(const std::string& key, const std::string& field,
                 const std::string& value) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (backend_ == "mongo") {
        auto cur = hgetall(key);
        cur[field] = value;
        Json obj = Json::objectOf({});
        for (auto& [f, v] : cur) obj.set(f, Json(v));
        mongoSet("hash:" + key, obj.dump());
        return;
    }
    if (backend_ == "redis") {
        redis_->hset("parlay:hash:" + key, field, value);
        return;
    }
    file_hashes_[key][field] = value;
    fileSave();
}

void Store::hdel(const std::string& key, const std::string& field) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (backend_ == "mongo") {
        auto cur = hgetall(key);
        cur.erase(field);
        Json obj = Json::objectOf({});
        for (auto& [f, v] : cur) obj.set(f, Json(v));
        mongoSet("hash:" + key, obj.dump());
        return;
    }
    if (backend_ == "redis") {
        redis_->hdel("parlay:hash:" + key, field);
        return;
    }
    file_hashes_[key].erase(field);
    fileSave();
}

std::map<std::string, std::string> Store::hgetall(const std::string& key) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (backend_ == "mongo") {
        auto v = mongoGet("hash:" + key);
        std::map<std::string, std::string> out;
        if (!v) return out;
        try {
            Json obj = Json::parse(*v);
            for (const auto& [f, val] : obj.asObject()) out[f] = val.asString();
        } catch (...) {
        }
        return out;
    }
    if (backend_ == "redis") return redis_->hgetall("parlay:hash:" + key);
    auto it = file_hashes_.find(key);
    if (it == file_hashes_.end()) return {};
    return it->second;
}

std::vector<std::string> Store::keys(const std::string& prefix) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (backend_ == "mongo") {
        auto all = mongo_->keysWithPrefix("kv:" + prefix);
        std::vector<std::string> out;
        for (auto& k : all) out.push_back(k.substr(3));
        return out;
    }
    if (backend_ == "redis") {
        auto raw = redis_->keys("parlay:kv:" + prefix + "*");
        std::vector<std::string> out;
        const std::string pfx = "parlay:kv:";
        for (auto& k : raw)
            out.push_back(k.rfind(pfx, 0) == 0 ? k.substr(pfx.size()) : k);
        return out;
    }
    std::vector<std::string> out;
    for (auto& [k, _] : file_kv_)
        if (k.rfind(prefix, 0) == 0) out.push_back(k);
    return out;
}

std::optional<std::string> Store::cacheGet(const std::string& key) {
    if (redis_) {
        std::string v;
        if (redis_->get("parlay:cache:" + key, v)) return v;
        return std::nullopt;
    }
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto it = mem_cache_.find(key);
    if (it == mem_cache_.end()) return std::nullopt;
    if (std::chrono::steady_clock::now() > it->second.second) {
        mem_cache_.erase(it);
        return std::nullopt;
    }
    return it->second.first;
}

void Store::cacheSet(const std::string& key, const std::string& value,
                     int ttl_sec) {
    if (redis_) {
        redis_->setEx("parlay:cache:" + key, value, ttl_sec);
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(mu_);
    mem_cache_[key] = {value, std::chrono::steady_clock::now() +
                                  std::chrono::seconds(ttl_sec)};
}

}  // namespace parlay
