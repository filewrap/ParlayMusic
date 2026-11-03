// store tests — Parlay shared durable store (A11).
// Offline: BSON round-trip, URI parsing, and full wire-protocol tests
// against fake in-process mongo/redis servers (no real server needed).
// Live: round-trips against real mongod/redis when PARLAY_TEST_MONGO_URI
// / PARLAY_TEST_REDIS_URL are set; skipped otherwise.
"use strict";

const assert = require("node:assert");
const net = require("node:net");
const test = require("node:test");

const {
  SharedStore,
  MongoClient,
  RedisClient,
  _bson,
  _parseMongoUri,
  _scram,
} = require("../src/store");

// ------------------------------------------------------------ BSON ----

test("bson: round-trip of every element type", () => {
  const d = _bson.docOf([
    _bson.elString("s", "hello"),
    _bson.elInt32("i32", -42),
    _bson.elInt64("i64", 1700000000123),
    _bson.elDouble("dbl", 2.5),
    _bson.elBool("b", true),
    _bson.elDoc("sub", _bson.docOf([_bson.elString("x", "y")])),
    _bson.elArrayOfDocs("arr", [
      _bson.docOf([_bson.elString("v", "one")]),
      _bson.docOf([_bson.elString("v", "two")]),
    ]),
    _bson.elRegex("rx", "^kv:"),
  ]);
  assert.equal(_bson.getString(d, "s"), "hello");
  assert.equal(_bson.getDouble(d, "i32"), -42);
  assert.equal(_bson.getDouble(d, "i64"), 1700000000123);
  assert.equal(_bson.getDouble(d, "dbl"), 2.5);
  const sub = _bson.getDoc(d, "sub");
  assert.equal(_bson.getString(sub, "x"), "y");
  const first = _bson.getFirstArrayDoc(d, "arr");
  assert.equal(_bson.getString(first, "v"), "one");
  const docs = _bson.getArrayDocs(d, "arr");
  assert.equal(docs.length, 2);
  assert.equal(_bson.getString(docs[1], "v"), "two");
  const seen = [];
  assert.equal(
    _bson.walk(d, (ty, k) => {
      seen.push(k);
      return true;
    }),
    true
  );
  assert.deepEqual(seen, ["s", "i32", "i64", "dbl", "b", "sub", "arr", "rx"]);
});

test("mongo uri parsing", () => {
  const u = _parseMongoUri("mongodb://127.0.0.1:27099/parlay");
  assert.deepEqual(u, {
    host: "127.0.0.1",
    port: 27099,
    db: "parlay",
    username: "",
    password: "",
    authSource: "parlay",
    authMechanism: null,
  });
  assert.equal(_parseMongoUri("mongodb://mongo:27017").db, "parlay");
  assert.equal(_parseMongoUri("mongodb://mongo:27017").authSource, "admin");
  // Credentials: parsed (pct-decoded), no longer rejected.
  const c = _parseMongoUri("mongodb://user:p%40ss%3Aw%2Frd@h:27017/db");
  assert.equal(c.username, "user");
  assert.equal(c.password, "p@ss:w/rd");
  assert.equal(c.authSource, "db");
  const a = _parseMongoUri(
    "mongodb://scram256:dummy-pass-256@127.0.0.1:27098/parlay_test?authSource=admin"
  );
  assert.equal(a.authSource, "admin");
  assert.equal(a.authMechanism, null);
  const m = _parseMongoUri(
    "mongodb://scram1:dummy-pass-1@127.0.0.1:27098/parlay_test?authMechanism=SCRAM-SHA-1"
  );
  assert.equal(m.authMechanism, "SCRAM-SHA-1");
  assert.throws(() => _parseMongoUri("mongodb://h:27017/db?authMechanism=PLAIN"), /authMechanism/);
  assert.throws(() => _parseMongoUri("mongodb+srv://x/y"), /srv/);
  assert.throws(() => _parseMongoUri("mongodb://a:27017,b:27017/db"), /multiple hosts/);
  assert.throws(() => _parseMongoUri("mongodb://:pass@h:27017/db"), /username/);
  assert.throws(() => _parseMongoUri("http://h:27017/db"), /mongodb:\/\//);
});



test("scram: SHA-1 worked example (RFC 5802 §5)", () => {
  // user "user", password "pencil".
  const c = new _scram.ScramClient("SCRAM-SHA-1", "user", "pencil", "fyko+d2lbbFgONRv9qkxdawL");
  assert.equal(c.clientFirst(), "n,,n=user,r=fyko+d2lbbFgONRv9qkxdawL");
  c.serverFirst("r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,s=QSXCR+Q6sek8bf92,i=4096");
  assert.equal(
    c.clientFinal(),
    "c=biws,r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,p=v0X8v3Bz2T0CJGbJQyF0X+HI4Ts="
  );
  c.serverFinal("v=rmF9pqV8S7suAoZWja4dJRkFsKQ=");
  // Rejections: bad server signature, server error, foreign nonce, bad salt.
  const bad = new _scram.ScramClient("SCRAM-SHA-1", "user", "pencil", "fyko+d2lbbFgONRv9qkxdawL");
  bad.serverFirst("r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,s=QSXCR+Q6sek8bf92,i=4096");
  assert.throws(() => bad.serverFinal("v=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="), /signature/);
  assert.throws(() => bad.serverFinal("e=invalid-proof"), /server error/);
  const foreign = new _scram.ScramClient("SCRAM-SHA-1", "user", "pencil", "fyko+d2lbbFgONRv9qkxdawL");
  assert.throws(
    () => foreign.serverFirst("r=OTHER_NONCE,s=QSXCR+Q6sek8bf92,i=4096"),
    /nonce/
  );
  const badsalt = new _scram.ScramClient("SCRAM-SHA-1", "user", "pencil", "fyko+d2lbbFgONRv9qkxdawL");
  assert.throws(
    () => badsalt.serverFirst("r=fyko+d2lbbFgONRv9qkxdawL,s=!!!,i=4096"),
    /iteration|salt|malformed/
  );
});

test("scram: SHA-256 worked example (RFC 7677 §3)", () => {
  // user "user", password "pencil". The server nonce contains ')'
  // and '$' — the parser must not choke.
  const c = new _scram.ScramClient("SCRAM-SHA-256", "user", "pencil", "rOprNGfwEbeRWgbNEkqO");
  assert.equal(c.clientFirst(), "n,,n=user,r=rOprNGfwEbeRWgbNEkqO");
  c.serverFirst(
    "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096"
  );
  assert.equal(
    c.clientFinal(),
    "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ="
  );
  c.serverFinal("v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=");
});

test("scram: mongoSha1Password digest", () => {
  // Verified live by A7: this digest authenticates against mongod
  // 8.0.4; the raw password does not.
  assert.equal(_scram.mongoSha1Password("scram256", "dummy-pass-256"), "e5f2bac50e85f6a08e894ac3270f7949");
});

test("bson: binary element round-trips through getBinary", () => {
  const d = _bson.docOf([_bson.elBinary("payload", Buffer.from("n,,n=user,r=x", "utf8"))]);
  assert.equal(_bson.getBinary(d, "payload").toString("utf8"), "n,,n=user,r=x");
  const d2 = _bson.docOf([_bson.elInt32("conversationId", 7)]);
  assert.equal(_bson.getInt32(d2, "conversationId"), 7);
});

// ------------------------------------------------------- fake mongo ----
// Minimal OP_MSG server: keeps { _id -> v } in memory, answers ping /
// find / update / delete with real BSON replies. Proves the wire
// client end to end without a mongod.

function readRegexAt(doc, vpos) {
  const end = doc.indexOf(0, vpos);
  return doc.subarray(vpos, end).toString("utf8");
}

function fakeMongoServer() {
  const docs = new Map(); // _id -> v string
  const server = net.createServer((sock) => {
    let buf = Buffer.alloc(0);
    sock.on("data", (d) => {
      buf = Buffer.concat([buf, d]);
      for (;;) {
        if (buf.length < 16) return;
        const len = buf.readInt32LE(0);
        if (buf.length < len) return;
        const msg = buf.subarray(0, len);
        buf = Buffer.from(buf.subarray(len));
        const requestId = msg.readInt32LE(4);
        const body = msg.subarray(21); // header(16) + flagBits(4) + kind(1)
        handleMongoCommand(sock, requestId, body, docs);
      }
    });
  });
  return new Promise((resolve) => {
    server.listen(0, "127.0.0.1", () => {
      resolve({ server, port: server.address().port, docs });
    });
  });
}

function opMsgReply(requestId, bodyDoc) {
  const total = 16 + 4 + 1 + bodyDoc.length;
  const header = Buffer.alloc(16);
  header.writeInt32LE(total, 0);
  header.writeInt32LE(requestId, 4);
  header.writeInt32LE(0, 8);
  header.writeInt32LE(2013, 12);
  const flags = Buffer.alloc(4);
  return Buffer.concat([header, flags, Buffer.from([0]), bodyDoc]);
}

function handleMongoCommand(sock, requestId, body, docs) {
  // The command name is the first key of the body document.
  let cmd = null;
  _bson.walk(body, (ty, k) => {
    cmd = k;
    return false;
  });
  let reply;
  if (cmd === "ping") {
    reply = _bson.docOf([_bson.elDouble("ok", 1)]);
  } else if (cmd === "find") {
    const filter = _bson.getDoc(body, "filter");
    let matched = [];
    if (filter) {
      // _id equality or _id regex?
      let idEq = null;
      let idRx = null;
      _bson.walk(filter, (ty, k, vpos) => {
        if (k === "_id" && ty === 0x02) idEq = _bson.getString(filter, "_id");
        if (k === "_id" && ty === 0x0b) idRx = readRegexAt(filter, vpos);
        return true;
      });
      if (idEq !== null) {
        if (docs.has(idEq)) matched.push(idEq);
      } else if (idRx !== null) {
        const rx = new RegExp(idRx);
        for (const id of docs.keys()) if (rx.test(id)) matched.push(id);
      } else {
        matched = [...docs.keys()];
      }
    }
    const batch = matched.map((id) =>
      _bson.docOf([_bson.elString("_id", id), _bson.elString("v", docs.get(id))])
    );
    const cursor = _bson.docOf([
      _bson.elInt64("id", 0),
      _bson.elString("ns", "parlay.state"),
      _bson.elArrayOfDocs("firstBatch", batch),
    ]);
    reply = _bson.docOf([_bson.elDouble("ok", 1), _bson.elDoc("cursor", cursor)]);
  } else if (cmd === "update") {
    const updates = _bson.getArrayDocs(body, "updates");
    for (const upd of updates) {
      const q = _bson.getDoc(upd, "q");
      const u = _bson.getDoc(upd, "u");
      const id = q ? _bson.getString(q, "_id") : null;
      const set = u ? _bson.getDoc(u, "$set") : null;
      const v = set ? _bson.getString(set, "v") : null;
      if (id !== null && v !== null) docs.set(id, v);
    }
    reply = _bson.docOf([_bson.elDouble("ok", 1), _bson.elInt32("n", 1)]);
  } else if (cmd === "delete") {
    const deletes = _bson.getArrayDocs(body, "deletes");
    for (const del of deletes) {
      const q = _bson.getDoc(del, "q");
      const id = q ? _bson.getString(q, "_id") : null;
      if (id !== null) docs.delete(id);
    }
    reply = _bson.docOf([_bson.elDouble("ok", 1), _bson.elInt32("n", 1)]);
  } else {
    reply = _bson.docOf([_bson.elDouble("ok", 0), _bson.elString("errmsg", `unknown command ${cmd}`)]);
  }
  sock.write(opMsgReply(requestId, reply));
}

test("mongo wire protocol against a fake mongod", async () => {
  const { server, port } = await fakeMongoServer();
  try {
    const client = await MongoClient.connect(`mongodb://127.0.0.1:${port}/parlay`);
    assert.equal(await client.findValue("kv:a11-test"), null);
    await client.upsertValue("kv:a11-test", '{"version":1}');
    assert.equal(await client.findValue("kv:a11-test"), '{"version":1}');
    await client.upsertValue("kv:a11-other", "x");
    const keys = await client.keysWithPrefix("kv:a11-");
    assert.deepEqual(keys.sort(), ["kv:a11-other", "kv:a11-test"]);
    await client.deleteKey("kv:a11-test");
    assert.equal(await client.findValue("kv:a11-test"), null);
    client.destroy();
  } finally {
    server.close();
  }
});

// ------------------------------------------------------- fake redis ----
// Minimal RESP server: in-memory map, answers PING/AUTH/SELECT/GET/
// SET/DEL/KEYS. Proves the RESP client end to end without a redis.

function fakeRedisServer() {
  const data = new Map();
  const server = net.createServer((sock) => {
    let buf = Buffer.alloc(0);
    sock.on("data", (d) => {
      buf = Buffer.concat([buf, d]);
      for (;;) {
        const parsed = tryParseResp(buf);
        if (!parsed) return;
        buf = parsed.rest;
        const [cmd, ...args] = parsed.argv;
        let out;
        switch ((cmd || "").toUpperCase()) {
          case "PING":
            out = "+PONG\r\n";
            break;
          case "AUTH":
          case "SELECT":
            out = "+OK\r\n";
            break;
          case "GET": {
            const v = data.get(args[0]);
            out = v === undefined ? "$-1\r\n" : `$${Buffer.byteLength(v)}\r\n${v}\r\n`;
            break;
          }
          case "SET":
            data.set(args[0], args[1]);
            out = "+OK\r\n";
            break;
          case "DEL":
            out = `:${data.delete(args[0]) ? 1 : 0}\r\n`;
            break;
          case "KEYS": {
            const rx = new RegExp("^" + args[0].replace(/\*/g, ".*") + "$");
            const hit = [...data.keys()].filter((k) => rx.test(k));
            out = `*${hit.length}\r\n` + hit.map((k) => `$${Buffer.byteLength(k)}\r\n${k}\r\n`).join("");
            break;
          }
          default:
            out = `-ERR unknown command '${cmd}'\r\n`;
        }
        sock.write(out);
      }
    });
  });
  return new Promise((resolve) => {
    server.listen(0, "127.0.0.1", () => {
      resolve({ server, port: server.address().port, data });
    });
  });
}

function tryParseResp(buf) {
  // Parse one RESP array of bulk strings; null when incomplete.
  if (buf.length < 1 || buf[0] !== 0x2a) return null; // '*'
  const eol = buf.indexOf("\r\n");
  if (eol < 0) return null;
  const count = parseInt(buf.subarray(1, eol).toString(), 10);
  if (Number.isNaN(count)) return null;
  let pos = eol + 2;
  const argv = [];
  for (let i = 0; i < count; i++) {
    if (buf[pos] !== 0x24) return null; // '$'
    const eol2 = buf.indexOf("\r\n", pos);
    if (eol2 < 0) return null;
    const len = parseInt(buf.subarray(pos + 1, eol2).toString(), 10);
    if (Number.isNaN(len) || buf.length < eol2 + 2 + len + 2) return null;
    argv.push(buf.subarray(eol2 + 2, eol2 + 2 + len).toString("utf8"));
    pos = eol2 + 2 + len + 2;
  }
  return { argv, rest: Buffer.from(buf.subarray(pos)) };
}

test("redis RESP protocol against a fake redis", async () => {
  const { server, port } = await fakeRedisServer();
  try {
    const client = await RedisClient.connect(`redis://127.0.0.1:${port}/0`);
    await client.set("parlay:kv:a11-test", "hello");
    assert.equal(await client.get("parlay:kv:a11-test"), "hello");
    assert.equal(await client.get("parlay:kv:missing"), null);
    const keys = await client.keys("parlay:kv:a11-*");
    assert.deepEqual(keys, ["parlay:kv:a11-test"]);
    await client.del("parlay:kv:a11-test");
    assert.equal(await client.get("parlay:kv:a11-test"), null);
    client.destroy();
  } finally {
    server.close();
  }
});

// ------------------------------------------------------------ live ----

test("live: SharedStore round-trip against real mongod", async () => {
  const uri = process.env.PARLAY_TEST_MONGO_URI || "";
  if (!uri) {
    console.log("SKIP live mongo (PARLAY_TEST_MONGO_URI unset)");
    return;
  }
  const prevMongo = process.env.MONGO_URI;
  const prevRedis = process.env.REDIS_URL;
  process.env.MONGO_URI = uri;
  delete process.env.REDIS_URL;
  let store;
  try {
    store = await SharedStore.open();
    assert.ok(store, "store opens");
    assert.equal(store.backend, "mongo");
    await store.set("a11-test-room", '{"version":1}');
    assert.equal(await store.get("a11-test-room"), '{"version":1}');
    const keys = await store.keys("a11-test-");
    assert.ok(keys.includes("a11-test-room"), `keys: ${keys}`);
    await store.del("a11-test-room");
    assert.equal(await store.get("a11-test-room"), null);
  } finally {
    if (store) store.destroy();
    if (prevMongo === undefined) delete process.env.MONGO_URI;
    else process.env.MONGO_URI = prevMongo;
    if (prevRedis !== undefined) process.env.REDIS_URL = prevRedis;
  }
});

test("live: SCRAM-SHA-256 auth round-trip against real mongod", async () => {
  const uri = process.env.PARLAY_TEST_MONGO_AUTH_URI || "";
  if (!uri) {
    console.log("SKIP live mongo auth (PARLAY_TEST_MONGO_AUTH_URI unset)");
    return;
  }
  const client = await MongoClient.connect(uri);
  try {
    await client.upsertValue("kv:r-scram256-room", '{"version":1}');
    assert.equal(await client.findValue("kv:r-scram256-room"), '{"version":1}');
    await client.deleteKey("kv:r-scram256-room");
    assert.equal(await client.findValue("kv:r-scram256-room"), null);
  } finally {
    client.destroy();
  }
});

test("live: SCRAM-SHA-1 auth round-trip against real mongod", async () => {
  const uri = process.env.PARLAY_TEST_MONGO_AUTH_SHA1_URI || "";
  if (!uri) {
    console.log("SKIP live mongo auth SHA-1 (PARLAY_TEST_MONGO_AUTH_SHA1_URI unset)");
    return;
  }
  // Negotiated: SHA-256 first, falls back to SHA-1 for a SHA-1-only user.
  const client = await MongoClient.connect(uri);
  try {
    await client.upsertValue("kv:r-scram1-room", "v1");
    assert.equal(await client.findValue("kv:r-scram1-room"), "v1");
    await client.deleteKey("kv:r-scram1-room");
  } finally {
    client.destroy();
  }
  // Forced SHA-1.
  const forced = await MongoClient.connect(`${uri}?authMechanism=SCRAM-SHA-1`);
  try {
    await forced.upsertValue("kv:r-scram1-room", "v2");
    assert.equal(await forced.findValue("kv:r-scram1-room"), "v2");
    await forced.deleteKey("kv:r-scram1-room");
  } finally {
    forced.destroy();
  }
});

test("live: wrong password fails honestly at connect", async () => {
  const uri = process.env.PARLAY_TEST_MONGO_AUTH_WRONG_URI || "";
  if (!uri) {
    console.log("SKIP live mongo auth wrong-password (PARLAY_TEST_MONGO_AUTH_WRONG_URI unset)");
    return;
  }
  await assert.rejects(
    () => MongoClient.connect(uri),
    (e) => /Authentication failed|SCRAM authentication failed/.test(e.message)
  );
});

test("live: SharedStore round-trip against real redis", async () => {
  const url = process.env.PARLAY_TEST_REDIS_URL || "";
  if (!url) {
    console.log("SKIP live redis (PARLAY_TEST_REDIS_URL unset)");
    return;
  }
  const prevMongo = process.env.MONGO_URI;
  const prevRedis = process.env.REDIS_URL;
  delete process.env.MONGO_URI;
  process.env.REDIS_URL = url;
  let store;
  try {
    store = await SharedStore.open();
    assert.ok(store, "store opens");
    assert.equal(store.backend, "redis");
    await store.set("a11-test-room", '{"version":1}');
    assert.equal(await store.get("a11-test-room"), '{"version":1}');
    const keys = await store.keys("a11-test-");
    assert.ok(keys.includes("a11-test-room"), `keys: ${keys}`);
    await store.del("a11-test-room");
    assert.equal(await store.get("a11-test-room"), null);
  } finally {
    if (store) store.destroy();
    if (prevMongo !== undefined) process.env.MONGO_URI = prevMongo;
    else delete process.env.MONGO_URI;
    if (prevRedis === undefined) delete process.env.REDIS_URL;
    else process.env.REDIS_URL = prevRedis;
  }
});

test("live: SharedStore.open returns null when unconfigured", async () => {
  const prevMongo = process.env.MONGO_URI;
  const prevRedis = process.env.REDIS_URL;
  delete process.env.MONGO_URI;
  delete process.env.REDIS_URL;
  try {
    assert.equal(await SharedStore.open(), null);
  } finally {
    if (prevMongo !== undefined) process.env.MONGO_URI = prevMongo;
    if (prevRedis !== undefined) process.env.REDIS_URL = prevRedis;
  }
});
