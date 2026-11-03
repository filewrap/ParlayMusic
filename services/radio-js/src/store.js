// store.js — Parlay shared durable store (Node stdlib only, no deps).
//
// One shared store for ALL stateful Parlay services, so a queue written
// by one VC engine is readable by another after fail-over. Backend
// priority is uniform everywhere (same as core-cpp `Store`):
//
//   MONGO_URI set -> MongoDB, REDIS_URL set -> Redis, else JSON file.
//
// The JSON-file path is NOT implemented here: `SharedStore.open()`
// returns null when neither variable is set, and the service keeps its
// existing file code byte-for-byte (no regression). A configured
// backend that cannot be reached makes `open()` throw (fatal at
// startup — the service exits) — silently falling back to a local file
// while another engine writes to Mongo/Redis would split-brain the
// queue. (core-cpp itself falls through with a warning; its store is
// A7-owned and off-limits here, and its keys are core-internal.)
//
// Wire protocols are hand-rolled stdlib ports of core-cpp
// `src/store.cpp` (OP_MSG + minimal BSON; RESP), so no npm packages
// are needed. Mongo SCRAM auth IS implemented here (SCRAM-SHA-256,
// falling back to SCRAM-SHA-1 — the `crypto`-module client in the
// `scram` section below, ported from core-cpp `src/scram.{hpp,cpp}`):
// URIs with credentials authenticate against the authSource database
// (default: the URI database, else "admin"); `?authMechanism=`
// forces one mechanism. Compose Mongo has no auth, so this only
// matters for auth-required deployments.
//
// Key scheme (canonical, all services — matches core-cpp exactly):
//   Mongo: db from URI (default `parlay`), collection `state`,
//          document {_id: "kv:<key>", v: "<string value>"}.
//   Redis: key "parlay:kv:<key>" -> "<string value>".
//   Keys: vc:room:<chat_id>  canonical VC room JSON (see
//          ARCHITECTURE.md, "Shared durable store")
//          queue:votes       queue-rust votes map JSON
//          share:links       share-rust token -> track map JSON
//          spotify:state     spotify-rust state JSON
//          radio:state       radio-js {radios:[...]} JSON
//
// Writes are serialised through one promise queue per store, in call
// order; `flush()` awaits everything enqueued so far (used at shutdown
// so a graceful SIGTERM never loses the last mutation). Node has no
// synchronous sockets, so saves are async — the queue bounds the
// exposure to a hard kill landing between a mutation and the
// (millisecond) write completing; graceful shutdown always flushes.
//
// NOTE: this file is copied verbatim into vc-ts and radio-js (separate
// services, no shared node_modules). Keep the copies in sync.

"use strict";

const crypto = require("crypto");
const net = require("net");

const IO_TIMEOUT_MS = 5000;
const MONGO_COLLECTION = "state";

function stripCredentials(uri) {
  const at = uri.lastIndexOf("@");
  const scheme = uri.indexOf("://");
  if (at >= 0 && scheme >= 0) return uri.slice(0, scheme + 3) + "<credentials>@" + uri.slice(at + 1);
  return uri;
}

function pctDecode(input) {
  try {
    return decodeURIComponent(input);
  } catch {
    return null;
  }
}

// ---------------------------------------------------------------- SCRAM --
// SCRAM client side (RFC 5802 for SHA-1, RFC 7677 for SHA-256),
// ported from core-cpp `src/scram.{hpp,cpp}` (same vectors, same
// message shapes). Uses only Node's built-in `crypto` module
// (createHash / createHmac / pbkdf2Sync / randomBytes) — no new
// dependencies. Passwords are used as UTF-8 bytes with no SASLprep
// normalization (RFC 4013) — exact for ASCII passwords, the
// realistic case for MONGO_URI (same limitation as core-cpp).

/// MongoDB's SCRAM-SHA-1 password derivation: the server stores
/// SCRAM-SHA-1 credentials derived from hex(MD5(username +
/// ":mongo:" + password)) — the legacy MONGODB-CR digest — NOT from
/// the raw password (verified live against mongod 8.0.4 by A7: the
/// raw password fails, this digest authenticates). SCRAM-SHA-256
/// uses the raw password.
function mongoSha1Password(username, password) {
  return crypto.createHash("md5").update(`${username}:mongo:${password}`, "utf8").digest("hex");
}

function parseScramAttrs(msg) {
  const out = new Map();
  for (const part of msg.split(",")) {
    const eq = part.indexOf("=");
    if (eq <= 0) return null;
    out.set(part.slice(0, eq), part.slice(eq + 1));
  }
  return out.size > 0 ? out : null;
}

/// One instance = one authentication attempt; the caller drives the
/// saslStart / saslContinue exchange. Pure RFC: the caller supplies
/// the effective password (MongoDB's SHA-1 MD5-hex transform is
/// applied by MongoClient.authenticate, not here).
class ScramClient {
  /// nonceB64: fixed client nonce (base64 text) for tests; empty
  /// generates a random nonce. Used verbatim as the SCRAM "r=" value.
  constructor(mechanism, username, password, nonceB64 = "") {
    this.mechanism = mechanism; // "SCRAM-SHA-256" | "SCRAM-SHA-1"
    this.hash = mechanism === "SCRAM-SHA-256" ? "sha256" : "sha1";
    this.hashLen = mechanism === "SCRAM-SHA-256" ? 32 : 20;
    this.username = username;
    this.password = Buffer.isBuffer(password) ? password : Buffer.from(password, "utf8");
    // SCRAM username escaping: '=' -> "=3D", ',' -> "=2C" (RFC 5802 §5.1).
    const escaped = username.replace(/=/g, "=3D").replace(/,/g, "=2C");
    this.clientNonce = nonceB64 || crypto.randomBytes(18).toString("base64");
    this.clientFirstBare = `n=${escaped},r=${this.clientNonce}`;
    this.haveServerFirst = false;
  }

  /// Full client-first-message: "n,,n=<user>,r=<nonce>".
  clientFirst() {
    return `n,,${this.clientFirstBare}`;
  }

  /// Parse the server-first-message ("r=<nonce>,s=<salt>,i=<iters>").
  /// Verifies the server nonce starts with our client nonce, then
  /// derives the salted password (PBKDF2).
  serverFirst(serverFirst) {
    const attrs = parseScramAttrs(serverFirst);
    if (!attrs || !attrs.has("r") || !attrs.has("s") || !attrs.has("i")) {
      throw new Error(`malformed SCRAM server-first-message: ${serverFirst}`);
    }
    const fullNonce = attrs.get("r");
    // The server nonce must extend our client nonce (RFC 5802 §5.1).
    if (!fullNonce.startsWith(this.clientNonce)) {
      throw new Error("SCRAM server nonce does not start with client nonce");
    }
    const saltB64 = attrs.get("s");
    // Strict base64: Buffer.from() silently drops bad characters,
    // so re-encode and compare (padding-insensitive) instead of
    // trusting the decode.
    const salt = Buffer.from(saltB64, "base64");
    const norm = (s) => s.replace(/=+$/, "");
    if (salt.length === 0 || norm(salt.toString("base64")) !== norm(saltB64)) {
      throw new Error("SCRAM server-first-message has invalid base64 salt");
    }
    const iterations = Number(attrs.get("i"));
    if (!Number.isInteger(iterations) || iterations < 1) {
      throw new Error("SCRAM server-first-message has invalid iteration count");
    }
    // Cap iterations so a hostile server cannot make us PBKDF2-loop
    // forever (MongoDB defaults to 10,000).
    if (iterations > 1000000) {
      throw new Error("SCRAM iteration count unreasonably large");
    }
    this.saltedPassword = crypto.pbkdf2Sync(this.password, salt, iterations, this.hashLen, this.hash);
    this.fullNonce = fullNonce;
    const clientFinalWoProof = `c=biws,r=${this.fullNonce}`;
    this.authMessage = `${this.clientFirstBare},${serverFirst},${clientFinalWoProof}`;
    this.haveServerFirst = true;
  }

  /// Client-final-message: "c=biws,r=<full nonce>,p=<base64 proof>".
  /// ("biws" is base64("n,,").) Requires serverFirst() to have run.
  clientFinal() {
    if (!this.haveServerFirst) return "";
    const clientKey = crypto.createHmac(this.hash, this.saltedPassword).update("Client Key").digest();
    const storedKey = crypto.createHash(this.hash).update(clientKey).digest();
    const clientSig = crypto.createHmac(this.hash, storedKey).update(this.authMessage).digest();
    const proof = Buffer.from(clientKey.map((b, i) => b ^ clientSig[i]));
    return `c=biws,r=${this.fullNonce},p=${proof.toString("base64")}`;
  }

  /// Verify the server-final-message ("v=<base64 ServerSignature>"
  /// on success, "e=<error>" on failure).
  serverFinal(serverFinal) {
    if (!this.haveServerFirst) {
      throw new Error("SCRAM server-final received before server-first");
    }
    const attrs = parseScramAttrs(serverFinal);
    if (!attrs) throw new Error(`malformed SCRAM server-final-message: ${serverFinal}`);
    if (attrs.has("e")) {
      throw new Error(`SCRAM authentication failed (server error): ${attrs.get("e")}`);
    }
    if (!attrs.has("v")) {
      throw new Error("SCRAM server-final-message has no verifier (v=)");
    }
    const serverKey = crypto.createHmac(this.hash, this.saltedPassword).update("Server Key").digest();
    const expected = crypto.createHmac(this.hash, serverKey).update(this.authMessage).digest().toString("base64");
    if (attrs.get("v") !== expected) {
      throw new Error("SCRAM server signature mismatch (possible MITM)");
    }
  }
}

// ---------------------------------------------------------------- TCP ----
// Buffered connection with promise-based readExact/readLine. One
// request is in flight at a time (callers serialise via SharedStore's
// queue), so a single waiter slot is enough.

class TcpConn {
  constructor(sock) {
    this.sock = sock;
    this.buf = Buffer.alloc(0);
    this.waiting = null;
    this.error = null;
    sock.on("data", (d) => {
      this.buf = Buffer.concat([this.buf, d]);
      if (this.waiting) {
        const w = this.waiting;
        this.waiting = null;
        w();
      }
    });
    sock.on("error", (e) => this._fail(e));
    sock.on("close", () => {
      if (!this.error) this._fail(new Error("connection closed"));
    });
  }

  _fail(e) {
    if (!this.error) this.error = e;
    if (this.waiting) {
      const w = this.waiting;
      this.waiting = null;
      w();
    }
  }

  static connect(host, port) {
    return new Promise((resolve, reject) => {
      const sock = net.connect(port, host);
      const timer = setTimeout(() => {
        sock.destroy();
        reject(new Error(`connect timeout ${host}:${port}`));
      }, IO_TIMEOUT_MS);
      sock.once("connect", () => {
        clearTimeout(timer);
        sock.setTimeout(IO_TIMEOUT_MS);
        resolve(new TcpConn(sock));
      });
      sock.once("error", (e) => {
        clearTimeout(timer);
        reject(e);
      });
    });
  }

  async _waitFor(pred) {
    for (;;) {
      if (pred()) return;
      if (this.error) throw this.error;
      await new Promise((resolve) => {
        this.waiting = resolve;
      });
      if (this.error) throw this.error;
    }
  }

  async readExact(n) {
    await this._waitFor(() => this.buf.length >= n);
    const out = Buffer.from(this.buf.subarray(0, n));
    this.buf = Buffer.from(this.buf.subarray(n));
    return out;
  }

  async readLine() {
    for (;;) {
      const idx = this.buf.indexOf(0x0a);
      if (idx >= 0) {
        let line = this.buf.subarray(0, idx).toString("utf8");
        if (line.endsWith("\r")) line = line.slice(0, -1);
        this.buf = Buffer.from(this.buf.subarray(idx + 1));
        return line;
      }
      await this._waitFor(() => this.buf.indexOf(0x0a) >= 0);
    }
  }

  write(buf) {
    return new Promise((resolve, reject) => {
      this.sock.write(buf, (e) => (e ? reject(e) : resolve()));
    });
  }

  destroy() {
    this.sock.destroy();
  }
}

// ---------------------------------------------------------------- BSON ----
// Minimal BSON builder/parser for the value shapes the store uses:
// string, int32, int64, double, bool, document, array of docs, regex.
// Ported from core-cpp src/store.cpp (namespace bson).

function i32(v) {
  const b = Buffer.alloc(4);
  b.writeInt32LE(v, 0);
  return b;
}
function i64(v) {
  const b = Buffer.alloc(8);
  b.writeBigInt64LE(BigInt(v), 0);
  return b;
}
function cstr(s) {
  return Buffer.concat([Buffer.from(s, "utf8"), Buffer.from([0])]);
}
function wrapDoc(elements) {
  const body = Buffer.concat(elements);
  return Buffer.concat([i32(body.length + 5), body, Buffer.from([0])]);
}
function elString(key, value) {
  const vb = Buffer.from(value, "utf8");
  return Buffer.concat([Buffer.from([0x02]), cstr(key), i32(vb.length + 1), vb, Buffer.from([0])]);
}
function elInt32(key, value) {
  return Buffer.concat([Buffer.from([0x10]), cstr(key), i32(value)]);
}
function elInt64(key, value) {
  return Buffer.concat([Buffer.from([0x12]), cstr(key), i64(value)]);
}
function elDouble(key, value) {
  const b = Buffer.alloc(8);
  b.writeDoubleLE(value, 0);
  return Buffer.concat([Buffer.from([0x01]), cstr(key), b]);
}
function elBool(key, value) {
  return Buffer.concat([Buffer.from([0x08]), cstr(key), Buffer.from([value ? 1 : 0])]);
}
function elDoc(key, docBuf) {
  return Buffer.concat([Buffer.from([0x03]), cstr(key), docBuf]);
}
function elArrayOfDocs(key, docs) {
  const elements = docs.map((d, i) => elDoc(String(i), d));
  return Buffer.concat([Buffer.from([0x04]), cstr(key), wrapDoc(elements)]);
}
function elRegex(key, pattern) {
  return Buffer.concat([Buffer.from([0x0b]), cstr(key), cstr(pattern), cstr("")]);
}
function elBinary(key, buf) {
  // BSON binary, subtype 0 (generic). Used for the SCRAM
  // saslStart / saslContinue payloads.
  const len = Buffer.alloc(4);
  len.writeInt32LE(buf.length, 0);
  return Buffer.concat([Buffer.from([0x05]), cstr(key), len, Buffer.from([0]), buf]);
}
function docOf(fields) {
  return wrapDoc(fields);
}

// Walk a BSON document; fn(type, key, valueOffset) per element.
// Returning false stops the walk (not an error). Returns false when
// the document is malformed.
function walk(doc, fn) {
  if (doc.length < 5) return false;
  let pos = 4;
  while (pos < doc.length) {
    const ty = doc[pos];
    if (ty === 0) return true;
    const keyEnd = doc.indexOf(0, pos + 1);
    if (keyEnd < 0) return false;
    const key = doc.subarray(pos + 1, keyEnd).toString("utf8");
    const vpos = keyEnd + 1;
    let next;
    switch (ty) {
      case 0x01:
        next = vpos + 8;
        break;
      case 0x02:
      case 0x0d:
      case 0x0e: {
        if (vpos + 4 > doc.length) return false;
        next = vpos + 4 + doc.readInt32LE(vpos);
        break;
      }
      case 0x03:
      case 0x04: {
        if (vpos + 4 > doc.length) return false;
        next = vpos + doc.readInt32LE(vpos);
        break;
      }
      case 0x05: {
        if (vpos + 4 > doc.length) return false;
        next = vpos + 5 + doc.readInt32LE(vpos);
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
        const a = doc.indexOf(0, vpos);
        if (a < 0) return false;
        const b = doc.indexOf(0, a + 1);
        if (b < 0) return false;
        next = b + 1;
        break;
      }
      case 0x0f: {
        if (vpos + 4 > doc.length) return false;
        next = vpos + doc.readInt32LE(vpos);
        break;
      }
      case 0x10:
        next = vpos + 4;
        break;
      case 0x13:
        next = vpos + 16;
        break;
      default:
        return false;
    }
    if (next > doc.length) return false;
    if (!fn(ty, key, vpos)) return true;
    pos = next;
  }
  return false;
}

function readStringAt(doc, vpos) {
  if (vpos + 4 > doc.length) return null;
  const len = doc.readInt32LE(vpos);
  if (len <= 0 || vpos + 4 + len > doc.length) return null;
  return doc.subarray(vpos + 4, vpos + 4 + len - 1).toString("utf8");
}

function getString(doc, key) {
  let out = null;
  walk(doc, (ty, k, vpos) => {
    if (k === key && ty === 0x02) {
      out = readStringAt(doc, vpos);
      return false;
    }
    return true;
  });
  return out;
}

function getInt32(doc, key) {
  let out = null;
  walk(doc, (ty, k, vpos) => {
    if (k === key && ty === 0x10) {
      out = doc.readInt32LE(vpos);
      return false;
    }
    return true;
  });
  return out;
}

/// Raw bytes of the BSON binary field `key` (type 0x05), if present.
function getBinary(doc, key) {
  let out = null;
  walk(doc, (ty, k, vpos) => {
    if (k === key && ty === 0x05) {
      if (vpos + 5 > doc.length) return false;
      const len = doc.readInt32LE(vpos);
      if (len >= 0 && vpos + 5 + len <= doc.length) out = doc.subarray(vpos + 5, vpos + 5 + len);
      return false;
    }
    return true;
  });
  return out;
}

function getDouble(doc, key) {
  let out = null;
  walk(doc, (ty, k, vpos) => {
    if (k !== key) return true;
    if (ty === 0x01 && vpos + 8 <= doc.length) {
      out = doc.readDoubleLE(vpos);
      return false;
    }
    if (ty === 0x10) {
      out = doc.readInt32LE(vpos);
      return false;
    }
    if (ty === 0x12 && vpos + 8 <= doc.length) {
      out = Number(doc.readBigInt64LE(vpos));
      return false;
    }
    return true;
  });
  return out;
}

function getDoc(doc, key) {
  let out = null;
  walk(doc, (ty, k, vpos) => {
    if (k === key && ty === 0x03) {
      const len = doc.readInt32LE(vpos);
      if (len > 0 && vpos + len <= doc.length) out = doc.subarray(vpos, vpos + len);
      return false;
    }
    return true;
  });
  return out;
}

function getFirstArrayDoc(doc, key) {
  let out = null;
  walk(doc, (ty, k, vpos) => {
    if (k !== key || ty !== 0x04) return true;
    const len = doc.readInt32LE(vpos);
    if (len <= 0 || vpos + len > doc.length) return false;
    const arr = doc.subarray(vpos, vpos + len);
    walk(arr, (t2, k2, v2) => {
      if (k2 === "0" && t2 === 0x03) {
        const dlen = arr.readInt32LE(v2);
        if (dlen > 0 && v2 + dlen <= arr.length) out = arr.subarray(v2, v2 + dlen);
        return false;
      }
      return true;
    });
    return false;
  });
  return out;
}

function getArrayDocs(doc, key) {
  const out = [];
  walk(doc, (ty, k, vpos) => {
    if (k !== key || ty !== 0x04) return true;
    const len = doc.readInt32LE(vpos);
    if (len <= 0 || vpos + len > doc.length) return false;
    const arr = doc.subarray(vpos, vpos + len);
    walk(arr, (t2, _k2, v2) => {
      if (t2 === 0x03) {
        const dlen = arr.readInt32LE(v2);
        if (dlen > 0 && v2 + dlen <= arr.length) out.push(arr.subarray(v2, v2 + dlen));
      }
      return true;
    });
    return false;
  });
  return out;
}

// --------------------------------------------------------------- Redis ----
// Minimal RESP client (GET/SET/DEL/KEYS/PING + AUTH/SELECT), ported from
// core-cpp RedisClient.

class RedisClient {
  constructor(conn) {
    this.conn = conn;
  }

  static async connect(url) {
    let rest = url;
    if (!rest.startsWith("redis://")) throw new Error("redis URL must start with redis://");
    rest = rest.slice("redis://".length);
    let password = "";
    const at = rest.indexOf("@");
    if (at >= 0) {
      const userinfo = rest.slice(0, at);
      rest = rest.slice(at + 1);
      const colon = userinfo.indexOf(":");
      password = colon >= 0 ? userinfo.slice(colon + 1) : userinfo;
    }
    let host = "127.0.0.1";
    let port = 6379;
    let db = 0;
    const slash = rest.indexOf("/");
    if (slash >= 0) {
      db = parseInt(rest.slice(slash + 1), 10) || 0;
      rest = rest.slice(0, slash);
    }
    const colon = rest.lastIndexOf(":");
    if (colon >= 0) {
      host = rest.slice(0, colon);
      port = parseInt(rest.slice(colon + 1), 10);
      if (!port) throw new Error("bad redis port");
    } else if (rest) {
      host = rest;
    }
    let conn;
    try {
      conn = await TcpConn.connect(host, port);
    } catch (e) {
      throw new Error(`cannot connect to redis at ${host}:${port}: ${e.message}`);
    }
    const client = new RedisClient(conn);
    try {
      if (password) {
        const r = await client.command(["AUTH", password]);
        if (r && r.error) throw new Error(`redis AUTH failed: ${r.error}`);
      }
      if (db !== 0) {
        const r = await client.command(["SELECT", String(db)]);
        if (r && r.error) throw new Error(`redis SELECT failed: ${r.error}`);
      }
      await client.ping();
    } catch (e) {
      conn.destroy();
      throw e;
    }
    return client;
  }

  async ping() {
    const r = await this.command(["PING"]);
    if (!r || r.simple !== "PONG") throw new Error("redis PING failed");
  }

  async command(argv) {
    let wire = `*${argv.length}\r\n`;
    for (const a of argv) wire += `$${Buffer.byteLength(a)}\r\n${a}\r\n`;
    await this.conn.write(Buffer.from(wire, "utf8"));
    return this.readReply();
  }

  async readReply() {
    const line = await this.conn.readLine();
    if (!line) throw new Error("redis empty reply");
    const kind = line[0];
    const payload = line.slice(1);
    switch (kind) {
      case "+":
        return { simple: payload };
      case "-":
        return { error: payload };
      case ":":
        return { int: parseInt(payload, 10) || 0 };
      case "$": {
        const len = parseInt(payload, 10);
        if (Number.isNaN(len)) throw new Error("bad bulk length");
        if (len < 0) return { bulk: null };
        const buf = await this.conn.readExact(len);
        await this.conn.readExact(2); // CRLF
        return { bulk: buf.toString("utf8") };
      }
      case "*": {
        const count = parseInt(payload, 10);
        if (Number.isNaN(count)) throw new Error("bad array length");
        if (count < 0) return { array: [] };
        const items = [];
        for (let i = 0; i < count; i++) items.push(await this.readReply());
        return { array: items };
      }
      default:
        throw new Error(`unsupported RESP type: ${kind}`);
    }
  }

  async get(key) {
    const r = await this.command(["GET", key]);
    if (r.error) throw new Error(`redis GET failed: ${r.error}`);
    return r.bulk !== undefined ? r.bulk : null;
  }

  async set(key, value) {
    const r = await this.command(["SET", key, value]);
    if (r.error) throw new Error(`redis SET failed: ${r.error}`);
  }

  async del(key) {
    const r = await this.command(["DEL", key]);
    if (r.error) throw new Error(`redis DEL failed: ${r.error}`);
  }

  async keys(pattern) {
    const r = await this.command(["KEYS", pattern]);
    if (r.error) throw new Error(`redis KEYS failed: ${r.error}`);
    return (r.array || []).filter((x) => x.bulk !== undefined && x.bulk !== null).map((x) => x.bulk);
  }

  destroy() {
    this.conn.destroy();
  }
}

// --------------------------------------------------------------- Mongo ----
// Minimal MongoDB wire-protocol client (OP_MSG + BSON subset), ported
// from core-cpp MongoClient, including SCRAM-SHA-256 / SCRAM-SHA-1
// authentication (saslStart / saslContinue against the authSource
// database). Same negotiation as core-cpp (A7): the URI's
// ?authMechanism= when set, else SCRAM-SHA-256 first with a fall-back
// to SCRAM-SHA-1 when the server reports the mechanism unusable for
// the user. A wrong password fails at saslContinue with the server's
// AuthenticationFailed error — never a silent success.

function parseMongoUri(uri) {
  if (!uri.startsWith("mongodb://")) {
    if (uri.startsWith("mongodb+srv://")) {
      throw new Error(
        "mongodb+srv:// URIs are not supported by the minimal wire-protocol client (no DNS SRV lookup); use mongodb:// with an explicit host:port"
      );
    }
    throw new Error("mongodb URI must start with mongodb://");
  }
  let rest = uri.slice("mongodb://".length);
  // Query string first: authSource, authMechanism (others ignored).
  let query = "";
  const q = rest.indexOf("?");
  if (q >= 0) {
    query = rest.slice(q + 1);
    rest = rest.slice(0, q);
  }
  let authSource = "";
  let authMechanism = null;
  for (const pair of query.split("&")) {
    if (!pair) continue;
    const eq = pair.indexOf("=");
    const key = eq >= 0 ? pair.slice(0, eq) : pair;
    const value = eq >= 0 ? pair.slice(eq + 1) : "";
    const decoded = pctDecode(value);
    if (decoded === null) throw new Error("bad percent-escape in mongodb URI query");
    if (key === "authSource") {
      authSource = decoded;
    } else if (key === "authMechanism") {
      if (decoded !== "SCRAM-SHA-256" && decoded !== "SCRAM-SHA-1") {
        throw new Error(
          `unsupported authMechanism in mongodb URI: ${decoded} (only SCRAM-SHA-1 / SCRAM-SHA-256)`
        );
      }
      authMechanism = decoded;
    }
  }
  let db = "parlay";
  let dbpartEmpty = true;
  const slash = rest.indexOf("/");
  if (slash >= 0) {
    const dbpart = rest.slice(slash + 1);
    rest = rest.slice(0, slash);
    if (dbpart) {
      dbpartEmpty = false;
      const decoded = pctDecode(dbpart);
      if (decoded === null) throw new Error("bad percent-escape in mongodb database name");
      db = decoded;
    }
  }
  // authSource default: the database in the URI, else "admin".
  if (!authSource) authSource = dbpartEmpty ? "admin" : db;
  // Authority: [userinfo@]host[:port]. Userinfo ends at the LAST '@'
  // (a password may contain a percent-encoded '@', never a raw one).
  // Credentials are SCRAM-authenticated (see authenticate()).
  let username = "";
  let password = "";
  const at = rest.lastIndexOf("@");
  if (at >= 0) {
    const userinfo = rest.slice(0, at);
    rest = rest.slice(at + 1);
    if (userinfo) {
      const colon = userinfo.indexOf(":");
      const user = colon >= 0 ? userinfo.slice(0, colon) : userinfo;
      const pass = colon >= 0 ? userinfo.slice(colon + 1) : "";
      username = pctDecode(user);
      password = pctDecode(pass);
      if (username === null || password === null) {
        throw new Error("bad percent-escape in mongodb credentials");
      }
      if (!username) throw new Error("mongodb URI has an empty username");
    }
  }
  if (!rest) throw new Error("mongodb URI has no host");
  if (rest.includes(",")) {
    throw new Error(
      "mongodb URIs with multiple hosts (replica sets) are not supported by the minimal wire-protocol client"
    );
  }
  let host;
  let port = 27017;
  if (rest.startsWith("[")) {
    const close = rest.indexOf("]");
    if (close < 0) throw new Error("malformed IPv6 host in mongodb URI");
    host = rest.slice(1, close);
    const tail = rest.slice(close + 1);
    if (tail) {
      if (!tail.startsWith(":")) throw new Error("malformed host:port in mongodb URI");
      port = parseInt(tail.slice(1), 10);
      if (!port) throw new Error("bad port in mongodb URI");
    }
  } else {
    const colon = rest.lastIndexOf(":");
    if (colon >= 0) {
      host = rest.slice(0, colon);
      port = parseInt(rest.slice(colon + 1), 10);
      if (!port) throw new Error("bad port in mongodb URI");
    } else {
      host = rest;
    }
  }
  if (!host || !port) throw new Error("bad host:port in mongodb URI");
  return { host, port, db, username, password, authSource, authMechanism };
}

class MongoClient {
  constructor(conn, host, port, db, username, password, authSource, authMechanism) {
    this.conn = conn;
    this.host = host;
    this.port = port;
    this.db = db;
    this.username = username;
    this.password = password;
    this.authSource = authSource;
    this.authMechanism = authMechanism;
    this.requestId = 1;
  }

  static async connect(uri) {
    const parsed = parseMongoUri(uri);
    let conn;
    try {
      conn = await TcpConn.connect(parsed.host, parsed.port);
    } catch (e) {
      throw new Error(`cannot connect to mongodb at ${parsed.host}:${parsed.port}: ${e.message}`);
    }
    const client = new MongoClient(
      conn,
      parsed.host,
      parsed.port,
      parsed.db,
      parsed.username,
      parsed.password,
      parsed.authSource,
      parsed.authMechanism
    );
    try {
      if (client.username) await client.authenticate();
      await client.ping();
    } catch (e) {
      conn.destroy();
      throw e;
    }
    return client;
  }

  /// SCRAM handshake (RFC 5802 / RFC 7677) via saslStart /
  /// saslContinue. Mechanism order: the URI's ?authMechanism= when
  /// set, else SCRAM-SHA-256 first with a fall-back to SCRAM-SHA-1
  /// if the server reports the mechanism unsupported (e.g. a user
  /// created with mechanisms ["SCRAM-SHA-1"] only). A wrong
  /// password fails here with the server's AuthenticationFailed
  /// error — never a silent success.
  async authenticate() {
    const mechanisms = this.authMechanism ? [this.authMechanism] : ["SCRAM-SHA-256", "SCRAM-SHA-1"];
    let lastError = "";
    for (let idx = 0; idx < mechanisms.length; idx++) {
      const mech = mechanisms[idx];
      // MongoDB quirk: SCRAM-SHA-1 credentials are derived from
      // hex(MD5(username + ":mongo:" + password)), not the raw
      // password (see mongoSha1Password). SCRAM-SHA-256 uses the
      // raw password.
      const effPassword =
        mech === "SCRAM-SHA-1" ? mongoSha1Password(this.username, this.password) : this.password;
      const sc = new ScramClient(mech, this.username, effPassword);
      // saslStart: payload is BinData(0, <raw client-first bytes>).
      const start = docOf([
        elInt32("saslStart", 1),
        elString("mechanism", mech),
        elBinary("payload", Buffer.from(sc.clientFirst(), "utf8")),
        elBool("autoAuthorize", true),
        elDoc("options", docOf([elBool("skipEmptyExchange", true)])),
        elString("$db", this.authSource),
      ]);
      let reply;
      try {
        reply = await this.runCommandRaw(start);
      } catch (e) {
        throw new Error(`saslStart transport failed: ${e.message}`);
      }
      if (getDouble(reply, "ok") !== 1) {
        lastError = `saslStart (${mech}) failed: ${getString(reply, "errmsg") || ""}`;
        // Fall through to the next mechanism when negotiating: any
        // saslStart failure means this mechanism is unusable for
        // this user on this server. A genuinely wrong password fails
        // at saslContinue, not here, so this cannot mask a bad
        // password.
        if (!this.authMechanism && idx + 1 < mechanisms.length) continue;
        throw new Error(lastError);
      }
      const conversationId = getInt32(reply, "conversationId");
      const serverFirstBuf = getBinary(reply, "payload");
      if (conversationId === null || serverFirstBuf === null) {
        throw new Error("saslStart reply missing conversationId/payload");
      }
      const serverFirst = serverFirstBuf.toString("utf8");
      try {
        sc.serverFirst(serverFirst);
      } catch (e) {
        throw new Error(`SCRAM server-first rejected: ${e.message}`);
      }
      const cont = docOf([
        elInt32("saslContinue", 1),
        elInt32("conversationId", conversationId),
        elBinary("payload", Buffer.from(sc.clientFinal(), "utf8")),
        elString("$db", this.authSource),
      ]);
      try {
        reply = await this.runCommandRaw(cont);
      } catch (e) {
        throw new Error(`saslContinue transport failed: ${e.message}`);
      }
      if (getDouble(reply, "ok") !== 1) {
        // Wrong password lands here (code 18, AuthenticationFailed).
        throw new Error(
          `SCRAM authentication failed for user '${this.username}' (${mech}): ${getString(reply, "errmsg") || ""}`
        );
      }
      const serverFinalBuf = getBinary(reply, "payload");
      if (serverFinalBuf === null) {
        throw new Error("saslContinue reply missing payload");
      }
      try {
        sc.serverFinal(serverFinalBuf.toString("utf8"));
      } catch (e) {
        throw new Error(`SCRAM server-final rejected: ${e.message}`);
      }
      return;
    }
    throw new Error(lastError);
  }

  async runCommandRaw(bodyBson) {
    // OP_MSG: header(16) + flagBits(4) + section kind(1) + body doc.
    const total = 16 + 4 + 1 + bodyBson.length;
    const header = Buffer.alloc(16);
    header.writeInt32LE(total, 0);
    header.writeInt32LE(this.requestId, 4);
    this.requestId = (this.requestId + 1) | 0;
    header.writeInt32LE(0, 8);
    header.writeInt32LE(2013, 12); // OP_MSG
    const msg = Buffer.concat([header, i32(0), Buffer.from([0]), bodyBson]);
    await this.conn.write(msg);
    const replyHeader = await this.conn.readExact(16);
    const len = replyHeader.readInt32LE(0);
    const op = replyHeader.readInt32LE(12);
    if (len < 16 || op !== 2013) throw new Error("mongodb bad reply header");
    const rest = await this.conn.readExact(len - 16);
    if (rest.length < 5 || rest[4] !== 0) throw new Error("mongodb unexpected reply sections");
    const body = rest.subarray(5);
    if (body.length < 4) throw new Error("mongodb short body");
    const blen = body.readInt32LE(0);
    if (blen > 0 && blen <= body.length) return Buffer.from(body.subarray(0, blen));
    return Buffer.from(body);
  }

  async runCommand(bodyBson) {
    const body = await this.runCommandRaw(bodyBson);
    if (getDouble(body, "ok") !== 1) {
      throw new Error(`mongodb command failed: ${getString(body, "errmsg") || ""}`);
    }
    return body;
  }

  async ping() {
    const body = docOf([elInt32("ping", 1), elString("$db", "admin")]);
    await this.runCommand(body);
  }

  /// Find one document's `v` field by _id. Null when absent.
  async findValue(key) {
    const filter = docOf([elString("_id", key)]);
    const body = docOf([
      elString("find", MONGO_COLLECTION),
      elDoc("filter", filter),
      elInt64("limit", 1),
      elString("$db", this.db),
    ]);
    const reply = await this.runCommand(body);
    const cur = getDoc(reply, "cursor");
    if (!cur) return null;
    const first = getFirstArrayDoc(cur, "firstBatch");
    if (!first) return null;
    return getString(first, "v");
  }

  async upsertValue(key, value) {
    const q = docOf([elString("_id", key)]);
    const set = docOf([elString("v", value)]);
    const u = docOf([elDoc("$set", set)]);
    const upd = docOf([elDoc("q", q), elDoc("u", u), elBool("upsert", true)]);
    const body = docOf([
      elString("update", MONGO_COLLECTION),
      elArrayOfDocs("updates", [upd]),
      elString("$db", this.db),
    ]);
    await this.runCommand(body);
  }

  async deleteKey(key) {
    const q = docOf([elString("_id", key)]);
    const del = docOf([elDoc("q", q), elInt32("limit", 1)]);
    const body = docOf([
      elString("delete", MONGO_COLLECTION),
      elArrayOfDocs("deletes", [del]),
      elString("$db", this.db),
    ]);
    await this.runCommand(body);
  }

  /// All _ids starting with `prefix` (full keys, `kv:` prefix NOT
  /// included — callers use the same key space as get/set/del).
  async keysWithPrefix(prefix) {
    const filter = docOf([elRegex("_id", `^${prefix}`)]);
    const body = docOf([
      elString("find", MONGO_COLLECTION),
      elDoc("filter", filter),
      elString("$db", this.db),
    ]);
    const reply = await this.runCommand(body);
    const cur = getDoc(reply, "cursor");
    if (!cur) return [];
    const out = [];
    for (const d of getArrayDocs(cur, "firstBatch")) {
      const id = getString(d, "_id");
      if (id !== null) out.push(id);
    }
    return out;
  }

  destroy() {
    this.conn.destroy();
  }
}

// --------------------------------------------------------- SharedStore ----

const mongoKey = (key) => `kv:${key}`;
const redisKey = (key) => `parlay:kv:${key}`;

class SharedStore {
  constructor(backend, client) {
    this.backend = backend;
    this.client = client;
    // Serialised op queue: every op runs after the previous one, in
    // call order, even when the caller does not await.
    this._tail = Promise.resolve();
  }

  /// Open the configured shared backend, or null when neither
  /// MONGO_URI nor REDIS_URL is set (caller keeps its JSON file).
  /// Throws when a configured backend cannot be reached.
  static async open() {
    const mongoUri = process.env.MONGO_URI || "";
    const redisUrl = process.env.REDIS_URL || "";
    if (mongoUri) {
      const client = await MongoClient.connect(mongoUri);
      console.log(`[store] backend=mongo mongo=${client.host}:${client.port} db=${client.db}`);
      return new SharedStore("mongo", client);
    }
    if (redisUrl) {
      const client = await RedisClient.connect(redisUrl);
      console.log(`[store] backend=redis redis=${stripCredentials(redisUrl)}`);
      return new SharedStore("redis", client);
    }
    return null;
  }

  describe() {
    if (this.backend === "mongo") {
      return `mongo ${this.client.host}:${this.client.port}/${this.client.db} state`;
    }
    return `redis ${stripCredentials(process.env.REDIS_URL || "")}`;
  }

  _enqueue(fn) {
    const run = this._tail.then(fn, fn);
    this._tail = run.then(
      () => {},
      () => {}
    );
    return run;
  }

  /// Await every operation enqueued so far (shutdown flush).
  flush() {
    return this._tail;
  }

  get(key) {
    return this._enqueue(() => {
      if (this.backend === "mongo") return this.client.findValue(mongoKey(key));
      return this.client.get(redisKey(key));
    });
  }

  set(key, value) {
    return this._enqueue(() => {
      if (this.backend === "mongo") return this.client.upsertValue(mongoKey(key), value);
      return this.client.set(redisKey(key), value);
    });
  }

  del(key) {
    return this._enqueue(() => {
      if (this.backend === "mongo") return this.client.deleteKey(mongoKey(key));
      return this.client.del(redisKey(key));
    });
  }

  /// Keys starting with `prefix`, without any backend prefix.
  keys(prefix) {
    return this._enqueue(async () => {
      if (this.backend === "mongo") {
        const full = await this.client.keysWithPrefix(mongoKey(prefix));
        return full.filter((k) => k.startsWith("kv:")).map((k) => k.slice(3));
      }
      const full = await this.client.keys(`${redisKey(prefix)}*`);
      return full.filter((k) => k.startsWith("parlay:kv:")).map((k) => k.slice("parlay:kv:".length));
    });
  }

  destroy() {
    this.client.destroy();
  }
}

module.exports = {
  SharedStore,
  MongoClient,
  RedisClient,
  TcpConn,
  // Exported for unit tests.
  _bson: {
    docOf,
    elString,
    elInt32,
    elInt64,
    elDouble,
    elBool,
    elDoc,
    elArrayOfDocs,
    elRegex,
    elBinary,
    walk,
    getString,
    getDouble,
    getInt32,
    getBinary,
    getDoc,
    getFirstArrayDoc,
    getArrayDocs,
  },
  _parseMongoUri: parseMongoUri,
  _scram: { ScramClient, mongoSha1Password },
};
