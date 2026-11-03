# Parlay — Install guide (manual / VPS, repo checkout)

Parlay is a Telegram music bot: a C++20 core + a unified Go backend
talking over one Unix Domain Socket, with a persistent yt-dlp daemon
and an embedded bbolt store — no inter-service HTTP, no Mongo/Redis.
See `FINAL-ARCHITECTURE.md` for the locked architecture. This guide
covers the **repo-checkout install** on a plain Linux VPS.

> The v0.2.0 release tarball installs the **old** 12-service topology
> and predates this architecture — do not use it for a fresh install.
> There is no newer release tarball yet; build from the repo (main at
> `f1a7211`, Phase 3). `install.sh` / `start.sh` / `stop.sh` /
> `status.sh` still describe the old topology and are being rewritten
> — until then, start the two processes by hand as below.

## Prereqs

| Requirement | Detail |
|---|---|
| OS / arch | Ubuntu 22.04+ or Debian 12, **x86_64** (the tarball ships x86_64 binaries + `.so` libs) |
| RAM | 2 GB minimum (core + backend + yt-dlp daemon + slskd) |
| Disk | ~500 MB for the install, plus room for downloads under `data/` |
| Network | Outbound HTTPS (Telegram, YouTube/Spotify/NetEase, slskd upstream); **open UDP + direct TCP to Telegram DCs** if you want live voice chat (see Troubleshooting) |
| Tools | `curl`, `tar`, `python3` (+ `venv`/`pip`), `ffmpeg`, `go` 1.23+, `cmake`, `g++` (C++20) |
| Accounts | A Telegram bot token from [@BotFather](https://t.me/BotFather), Telegram `API_ID`/`API_HASH` from [my.telegram.org](https://my.telegram.org) (needed for the TDLib voice backend), a Soulseek account if you want Soulseek downloads |

Install the tools on Debian/Ubuntu:

```bash
sudo apt update && sudo apt install -y curl tar python3 python3-venv python3-pip ffmpeg libopus0 cmake g++
# libopus0 is explicit (Phase 5): libparlay_dsp.so links libopus.so.0
# directly; ffmpeg normally pulls it in, but a static ffmpeg would not.
# go 1.23+ (https://go.dev/dl/ — no distro package is new enough on 22.04):
curl -LO https://go.dev/dl/go1.23.0.linux-amd64.tar.gz
sudo tar -C /usr/local -xzf go1.23.0.linux-amd64.tar.gz
export PATH=$PATH:/usr/local/go/bin
```

## Install from the repo

```bash
git clone https://github.com/filewrap/ParlayMusic.git && cd ParlayMusic
cp .env.example .env   # fill in BOT_TOKEN, API_ID, API_HASH, OWNER_ID, PARLAY_TOKEN
cp parlay.toml.example parlay.toml   # optional: defaults apply without it

# yt-dlp daemon venv (the backend starts the daemon from this venv at boot)
python3 -m venv scripts/yt-dlp-venv
scripts/yt-dlp-venv/bin/pip install -U yt-dlp

# build both processes
cd services/backend-go && go build -o parlay-backend ./cmd/backend && cd ../..
cd services/core-cpp && cmake -B build && cmake --build build && cd ../..
```

Then start, backend first (it creates the UDS socket the core
connects to), core second:

```bash
./services/backend-go/parlay-backend &
./services/core-cpp/build/parlay-core
```

Stop with Ctrl-C / `kill` on each process (no `stop.sh` yet — the
launcher rewrite is pending). Both processes log to stdout/stderr;
redirect to files if you want persistent logs.

## Manual notes

- The backend warms the yt-dlp daemon at boot (first resolve never
  forks). If the warm fails, startup continues with the native
  resolvers only and yt-dlp stays down until restart.
- The backend opens the bbolt store at `data/parlay.db` (0600 in a
  0700 `data/` dir) and listens on `data/parlay-backend.sock`.
  Override the data dir with `PARLAY_DATA_DIR` / `backend.data_dir`
  in `parlay.toml`.
- TDLib sessions live under `data/` too: `data/tdlib-bot` (bot
  session, token auth) and `data/tdlib` (assistant session), both
  0700. First assistant login: `scripts/tdlib_login.py` (phone →
  code → 2FA, or QR).
- slskd (Soulseek daemon) is opt-in: fetch the slskd release for
  linux-x64 from https://github.com/slskd/slskd/releases and place
  the `slskd` binary on PATH (or set `SLSKD_URL` to a running
  instance).
- The core's legacy `:8080` HTTP server still starts at boot but
  nothing uses it (FINAL-ARCHITECTURE.md §6-R1 schedules its
  deletion); core↔backend traffic is UDS-only.

## Ports / sockets

No inter-service HTTP ports exist anymore. The only listeners are:

| Thing | Address | Notes |
|---|---|---|
| core ↔ backend IPC | `data/parlay-backend.sock` (UDS) | backend creates/listens; core connects; `PARLAY_UDS_PATH` overrides |
| bbolt store | `data/parlay.db` (file, not a port) | single embedded store for all state |
| core legacy HTTP | `:8080` | leftover server, unused, slated for deletion (§6-R1) |
| slskd | 5030 / 5031 | Soulseek daemon web UI+API (HTTP / HTTPS), opt-in |

## State and logs

All state lives in the single bbolt store **`data/parlay.db`** —
rooms, queues, votes, shares, Spotify state, settings, radio state
(7 buckets). There is no Mongo/Redis and no per-service JSON store
anymore; `MONGO_URI` / `REDIS_URL` in `.env` are legacy and ignored.
`data/` (store, downloads, TDLib sessions) is untouched by an
upgrade — back it up before one.

When run by hand, both processes log to stdout/stderr — redirect
them yourself (`>> logs/backend.log 2>&1`, `>> logs/core.log 2>&1`)
until the launcher rewrite restores `logs/<service>.log` files.

## systemd

> Pending the launcher rewrite: `start.sh` / `stop.sh` do not exist
> in their new form yet, so the unit below is a sketch, not a tested
> unit. The shape that fits the new layout is a oneshot unit that
> stays "active" after the start script exits, with `stop.sh` as the
> stop action — the same shape as before, because the start script
> still backgrounds processes itself and then exits (there are two
> processes + the daemon now, not twelve services).

```ini
# /etc/systemd/system/parlay.service
[Unit]
Description=Parlay Telegram music bot
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
RemainAfterExit=yes
WorkingDirectory=/opt/parlay
User=parlay
ExecStart=/opt/parlay/start.sh
ExecStop=/opt/parlay/stop.sh
Restart=on-failure
RestartSec=10

[Install]
WantedBy=multi-user.target
```

```bash
sudo cp -r ParlayMusic /opt/parlay
sudo useradd -r -m -d /opt/parlay parlay 2>/dev/null || true
sudo chown -R parlay:parlay /opt/parlay
sudo systemctl daemon-reload
sudo systemctl enable --now parlay
systemctl status parlay
```

Notes:

- `Restart=on-failure` here only retries a *failed start* (a oneshot
  unit has no running process to restart). If a process dies later,
  nothing restarts it automatically — restart the bot by hand and
  check the logs first.
- Run as a non-root user (`User=parlay`); the install dir must be
  writable by that user (`data/`, `logs/`, downloads).
- If you would rather have systemd supervise the core in the
  foreground instead, run `services/core-cpp/build/parlay-core`
  directly with `Type=simple` in a *separate* unit — but then it
  does not start the backend, so do not mix the two approaches in
  one unit.

## Updating

1. Stop the bot (kill both processes, backend + core).
2. `git pull` and rebuild both processes (see "Install from the
   repo" above).
3. Back up `data/` first (the bbolt store + TDLib sessions).
4. Start again, backend first, core second.

Your `.env` and `parlay.toml` are preserved — they live outside the
build dirs. `data/` (state, queues, downloads) is also untouched by
an upgrade. If a release changes the config template, diff
`.env.example` / `parlay.toml.example` against your files and add
any new variables by hand.

## Troubleshooting

**Bot token invalid / `getMe` fails.**
Check the token first, without the bot running:

```bash
curl -s "https://api.telegram.org/bot<BOT_TOKEN>/getMe"
```

`{"ok":false,... "error_code":401}` means the token is wrong or
revoked — get a fresh one from @BotFather and put it in `.env` as
`BOT_TOKEN`. `{"ok":true,...}` means the token is fine and the problem
is elsewhere (proxy/firewall blocking `api.telegram.org`, or the core
reading a stale `.env` — restart after every `.env` edit). If the VPS
needs an HTTP proxy for outbound traffic, the core honours the
standard `https_proxy`/`HTTPS_PROXY` environment variables.

**UDS socket already in use / backend won't start.**
The backend creates `data/parlay-backend.sock` at startup. If a
stale backend is still running (or a previous one died without
removing the socket), the new backend fails to listen. Kill any
stale `parlay-backend` process first; the socket file itself is
removed and recreated on a clean start. The legacy core `:8080`
HTTP port can also clash — see "Port already in use" below.

**Port already in use.**
Only the legacy core `:8080` HTTP server binds a TCP port now (and
it is slated for deletion). A failed bind shows in the core's log
("address already in use"). Find the squatter with
`ss -tlnp | grep 8080` and stop it. There are no per-service ports
to change anymore — core↔backend traffic is the UDS socket above.

**Voice chat never joins / `[voice] backend=unavailable`.**
Live voice needs all three, and the core logs exactly which one is
missing at startup:

1. An **assistant session**: `API_ID`/`API_HASH` (or `TD_API_ID`/
   `TD_API_HASH`) plus a pre-authorized session dir
   (`data/tdlib`, or `TD_SESSION_DIR`), or first-auth via
   `TD_PHONE`+`TD_CODE` (or `TD_QR=1` for QR login).
   The bot token alone cannot join a voice chat.
2. **Build-time linked libraries**: TDLib 1.8.69 is linked into the
   core at build time and ntgcalls into the backend via cgo — there
   are no `NTGCALLS_LIB` / `TDJSON_LIB` runtime env vars anymore.
   If the build could not find them, the binaries fail at build
   time, not at runtime; a missing `libtdjson.so.1.8.69` is a build
   problem (see `services/core-cpp/CMakeLists.txt`, `-DTDJSON_LIB=`).
3. An **open network**: direct TCP to Telegram DCs plus working
   UDP/RTP. Sandboxes and locked-down VPS firewalls block this —
   the sandbox this release was built in cannot do it at all, so a
   failed join there proves nothing about your host. If UDP is
   blocked, voice cannot work no matter what the config says.

**`/play` works but there is no audio.**
Same causes as above — the backend resolved the track but no voice
backend is attached. Check the core's log for the `[voice]` line.

**slskd fails / Soulseek downloads never start.**
slskd needs a **Soulseek account** (username + password) — set them in
slskd's own config/web UI on first run (`http://<host>:5030`). Without
a login slskd runs but cannot search or download, and the backend
fails those requests honestly. Also note Soulseek search is paced
(`SLSKD_SEARCH_MIN_INTERVAL_SECS`, default 10 s) — flooding it gets
the account throttled by the network.

**`PARLAY_TOKEN` missing at startup.**
The core refuses to start without `PARLAY_TOKEN` (and `BOT_TOKEN`)
— it is a required variable, not a warning. Set it in `.env`; the
backend reads the same file. There is no per-service token anymore.

**A process exits immediately.**
Read its log — the usual causes are a missing env var (named in the
log), a stale UDS socket (above), or a missing tool (`ffmpeg` for
the voice pipeline, `yt-dlp` in `scripts/yt-dlp-venv` for the
daemon).

**Still stuck?**
Attach the last 50 lines of the failing process's log (redact any
token first) when asking for help. Never paste your `.env`.
