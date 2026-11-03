# Parlay — Install guide (manual / VPS, no Docker)

Parlay is a Telegram music bot (C++ core + Go/Rust/TS/JS services). This
guide covers the **release-tarball install** on a plain Linux VPS. For
the Docker route instead, see `README.md` (Quick start) and
`docker-compose.yml`.

## Prereqs

| Requirement | Detail |
|---|---|
| OS / arch | Ubuntu 22.04+ or Debian 12, **x86_64** (the tarball ships x86_64 binaries + `.so` libs) |
| RAM | 2 GB minimum (core + 11 services + slskd) |
| Disk | ~500 MB for the install, plus room for downloads under `data/` |
| Network | Outbound HTTPS (Telegram Bot API, YouTube/Spotify/NetEase, slskd upstream); **open UDP + direct TCP to Telegram DCs** if you want live voice chat (see Troubleshooting) |
| Tools | `curl`, `tar`, `python3` (+ `venv`/`pip`), `node` 18+, `ffmpeg`, `yt-dlp` |
| Accounts | A Telegram bot token from [@BotFather](https://t.me/BotFather), Telegram `API_ID`/`API_HASH` from [my.telegram.org](https://my.telegram.org) (needed for the TDLib voice backend), a Soulseek account if you want Soulseek downloads |

Install the tools on Debian/Ubuntu:

```bash
sudo apt update && sudo apt install -y curl tar python3 python3-venv python3-pip ffmpeg
# node 18+ (nodesource) and yt-dlp:
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash - && sudo apt install -y nodejs
sudo pip install --break-system-packages -U yt-dlp   # or: sudo apt install -y yt-dlp
```

## One-click path (release tarball)

```bash
curl -LO https://github.com/filewrap/ParlayMusic/releases/download/v0.2.0/parlay-v0.2.0-linux-x86_64.tar.gz
tar xzf parlay-v0.2.0-linux-x86_64.tar.gz
cd parlay-v0.2.0-linux-x86_64 && ./install.sh
```

Then configure and start:

```bash
cp .env.example .env   # fill in BOT_TOKEN, API_ID, API_HASH, OWNER_ID, PARLAY_TOKEN
./start.sh
./status.sh            # check every service answers /healthz
./stop.sh              # stop everything
```

`install.sh` is idempotent and safe to re-run: it performs first-time
setup (fetches the slskd daemon from upstream — slskd is **not** in the
tarball — prepares runtime directories, and installs the downloaders'
Python dependencies). It never overwrites an existing `.env`.

## Manual path (same tarball, no install.sh)

```bash
curl -LO https://github.com/filewrap/ParlayMusic/releases/download/v0.2.0/parlay-v0.2.0-linux-x86_64.tar.gz
tar xzf parlay-v0.2.0-linux-x86_64.tar.gz
cd parlay-v0.2.0-linux-x86_64
cp .env.example .env   # fill in BOT_TOKEN, API_ID, API_HASH, OWNER_ID, PARLAY_TOKEN

# downloaders' Python deps (isolated venv, nothing system-wide)
python3 -m venv services/downloaders/.venv
services/downloaders/.venv/bin/pip install -r services/downloaders/requirements.txt

# slskd (Soulseek daemon) — fetched by install.sh; manually:
# download the slskd release for linux-x64 from https://github.com/slskd/slskd/releases
# and place the `slskd` binary on PATH (or set SLSKD_URL to a running instance).

# make sure the native libs are findable (start.sh sets this for you)
export LD_LIBRARY_PATH="$PWD/lib:$LD_LIBRARY_PATH"
export TDJSON_LIB="$PWD/lib/libtdjson.so.1.8.69"
export NTGCALLS_LIB="$PWD/lib/libntgcalls.so"

./start.sh
```

The JS services (`vc-ts`, `gui-js`, `radio-js`) use Node's stdlib only —
no `npm install` needed. The Go/Rust binaries in `bin/` are prebuilt;
nothing needs compiling on the host.

## Ports

All services listen on localhost by default; only the ones you publish
are reachable from outside. The core's HTTP API is on `:8080`.

| Service | Port | Notes |
|---|---|---|
| core-cpp | 8080 | Bot core + HTTP API (`/healthz`, `/api/now-playing`, `/api/queue`) |
| vc-go | 8001 | Go VC engine |
| vc-ts | 8002 | TS VC engine |
| vc-rust | 8003 | Rust VC engine |
| audio-fx-rust | 8004 | Audio effects |
| share-rust | 8005 | Anonymous share links |
| queue-rust | 8006 | Voting queue |
| spotify-rust | 8007 | Spotify watch / classifier |
| radio-js | 8008 | Internet radio |
| downloaders | 8009 | Python download workers |
| netease-go | 8010 | NetEase search/download |
| gui-js | 3000 | Web GUI / Mini App player (the one port you normally publish) |
| slskd | 5030 / 5031 | Soulseek daemon web UI+API (HTTP / HTTPS) |
| mongo / redis | 27017 / 6379 | **Docker Compose only** — the tarball install does not use them (see below) |

## State, logs, and Mongo

The tarball install uses **JSON-file stores** (under `data/`, one subdir
per service) — MongoDB and Redis are Compose-only and are **not**
required here. `MONGO_URI` / `REDIS_URL` in `.env` are ignored on this
path; Mongo support is aside upstream, so do not point the tarball
install at a Mongo server expecting shared state.

Logs live under `logs/` in the install directory (one file per service,
written by `start.sh`). Check them first when something misbehaves:

```bash
tail -f logs/core.log        # core-cpp
tail -f logs/vc-go.log       # any other service: logs/<service>.log
```

## systemd

`start.sh` backgrounds every service itself (plain shell `&`, one PID
file per service in `run/`, logs in `logs/`) and then exits, so the unit
must **not** be `Type=simple` — systemd would see `start.sh` exit,
conclude the service died, and `Restart=on-failure` would loop forever
re-running it. `Type=forking` is wrong too: there is no single daemon
PID (there are twelve). The coherent shape is a oneshot unit that stays
"active" after the start script exits, with `stop.sh` as the stop
action. Because the services are plain background children (not
detached with `nohup`/`setsid`), they stay in the unit's cgroup while
it is active; on stop, `stop.sh` kills them by PID file and systemd
then reaps any stragglers:

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
sudo cp -r parlay-v0.2.0-linux-x86_64 /opt/parlay
sudo useradd -r -m -d /opt/parlay parlay 2>/dev/null || true
sudo chown -R parlay:parlay /opt/parlay
sudo systemctl daemon-reload
sudo systemctl enable --now parlay
systemctl status parlay
```

Notes:

- `Restart=on-failure` here only retries a *failed start* (a oneshot
  unit has no running process to restart). If a service dies later,
  nothing restarts it automatically — run `./stop.sh` then `./start.sh`
  (or `systemctl restart parlay`) to bring it back; check `logs/` and
  `./status.sh` first.
- Run as a non-root user (`User=parlay`); the install dir must be
  writable by that user (`data/`, `logs/`, downloads).
- If you would rather have systemd supervise the core in the
  foreground instead, run `bin/parlay-core` directly with
  `Type=simple` in a *separate* unit — but then it does not start
  the other eleven services, so do not mix the two approaches in
  one unit.

## Updating

1. Download the new release tarball and verify its checksum
   (`sha256sum -c parlay-vX.Y.Z-linux-x86_64.tar.gz.sha256`).
2. Stop the bot: `./stop.sh` (or `systemctl stop parlay`).
3. Extract the new tarball **over** the install directory.
4. Re-run `./install.sh` (refreshes slskd / Python deps if needed).
5. Start again: `./start.sh` (or `systemctl start parlay`).

Your `.env` is preserved — `install.sh` never overwrites it, and the
tarball ships only `.env.example`. `data/` (state, queues, downloads)
is also untouched by an upgrade. If a release changes the config
template, diff `.env.example` against your `.env` and add any new
variables by hand.

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

**Port already in use.**
`./status.sh` shows which services are up; a failed bind is also in
`logs/<service>.log` ("address already in use"). Find the squatter
with `ss -tlnp | grep <port>` and stop it, or change the service's
`PORT` in `.env` (and the matching `*_URL` the core uses to reach it —
see `.env.example`; the port table above is the source of truth).

**Voice chat never joins / `[voice] backend=unavailable`.**
Live voice needs all four, and the core logs exactly which one is
missing at startup:

1. An **assistant session**: `API_ID`/`API_HASH` (or `TD_API_ID`/
   `TD_API_HASH`) plus a pre-authorized `TD_SESSION_DIR`, or
   first-auth via `TD_PHONE`+`TD_CODE` (or `TD_QR=1` for QR login).
   The bot token alone cannot join a voice chat.
2. `NTGCALLS_LIB` pointing at `lib/libntgcalls.so` (the tarball ships
   it; `start.sh` sets this — if you start binaries by hand, export
   it yourself).
3. `TDJSON_LIB` pointing at `lib/libtdjson.so.1.8.69` (same).
4. An **open network**: direct TCP to Telegram DCs plus working
   UDP/RTP. Sandboxes and locked-down VPS firewalls block this —
   the sandbox this release was built in cannot do it at all, so a
   failed join there proves nothing about your host. If UDP is
   blocked, voice cannot work no matter what the config says.

**`/play` works but there is no audio.**
Same causes as above — the engine resolved the track but no voice
backend is attached. Check `logs/core.log` for the `[voice]` line.

**slskd fails / Soulseek downloads never start.**
slskd needs a **Soulseek account** (username + password) — set them in
slskd's own config/web UI on first run (`http://<host>:5030`). Without
a login slskd runs but cannot search or download, and the downloaders
fail those requests honestly. Also note Soulseek search is paced
(`SLSKD_SEARCH_MIN_INTERVAL_SECS`, default 10 s) — flooding it gets
the account throttled by the network.

**`PARLAY_TOKEN` warnings at startup.**
The core warns when `PARLAY_TOKEN` is empty. Set the same value in
`.env` for all services (the core and every engine read it from the
same file on this path). Empty means IPC auth is off — fine for a
local test, not for a public host.

**A service exits immediately.**
Read its log in `logs/` — the usual causes are a missing env var
(named in the log), a port clash (above), or a missing tool
(`ffmpeg`/`yt-dlp` for the engines, `node` for the JS services).

**Still stuck?**
Run `./status.sh`, then attach the last 50 lines of the failing
service's `logs/<service>.log` (redact any token first) when asking
for help. Never paste your `.env`.
