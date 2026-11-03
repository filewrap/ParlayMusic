# Parlay — v0.3.0 Release (2-process engine, Phases 1–6)

Date: 2026-11-04. Branch: `main`. Tag: `v0.3.0`.

One-click install (VPS, Ubuntu 22.04+/Debian 12 x86_64):

```bash
curl -LO https://github.com/filewrap/ParlayMusic/releases/download/v0.3.0/parlay-v0.3.0-linux-x86_64.tar.gz
tar -xzf parlay-v0.3.0-linux-x86_64.tar.gz
cd parlay-v0.3.0-linux-x86_64 && ./install.sh && ./start.sh
```

This is the first release of the locked 2-process engine
(`FINAL-ARCHITECTURE.md`): C++20 core + unified Go backend over one
Unix Domain Socket, bbolt store, no Mongo/Redis, no inter-service
HTTP. All 6 phases: telemetry + benchmark, persistent IPC + dual-
TDLib core, unified backend (65 IPC actions), cache + lookahead
prefetch, parlay-dsp Rust engine + patched ntgcalls Opus (320k
ceiling, 192k default), TypeScript lossless Mini App with FLAC
streaming. `vc.join` deadlock fixed (65/65 dispatch). No initData
validation in-tree — the Mini App binds loopback-only by default;
put a trusted reverse proxy with TLS + initData check in front of
it on a VPS.

## Release artifact

- `release/parlay-v0.3.0-linux-x86_64.tar.gz` — 57,584,859 bytes (55M),
  26 tar entries.
- SHA256:
  `76faed638f8b5c699a65182ccd18b688c2ed4350eae54799095ddabf8a266c5f`
  (sidecar `.sha256` next to the tarball; `SHA256SUMS` inside it,
  verified).
- Contents: `bin/parlay-core`, `bin/parlay-backend`,
  `lib/libtdjson.so.1.8.69` (50,166,944 B, prebuilt) +
  `lib/libntgcalls.so` (98,123,704 B, PATCHED Phase 5 Opus build) +
  `lib/libparlay_dsp.so` (2,552,488 B, Rust DSP engine),
  `services/miniapp/dist/` (built bundle), `scripts/yt_dlp_worker.py`,
  `install.sh`, `start.sh`, `stop.sh`, `status.sh`, `.env.example`,
  `parlay.toml.example`, `README.md`, `INSTALL.md`, `VERSION`,
  `SHA256SUMS`. No `.env`, no tokens, no session files, no `refs/`,
  no build dirs, no venvs. slskd is NOT in the tarball —
  `install.sh` fetches it from upstream (pinned 0.26.2).
- Secret scan clean (package-release.sh step 7).

Expected release URL once published:
https://github.com/filewrap/ParlayMusic/releases/tag/v0.3.0

---

# Parlay — v0.2.0 Release (one-click install/start)

> **Superseded note (2026-11-03, Phase 3):** this report documents
> the v0.2.0 release, which shipped the **old** 12-service HTTP
> topology (12 services on ports 8001–8010/3000/8080, Mongo/Redis).
> That topology has since been deleted and replaced by the locked
> 2-process engine (core-cpp + backend-go over UDS, bbolt store —
> see `FINAL-ARCHITECTURE.md`, main at `f1a7211`). The one-click
> install below installs the old topology; do not use it for a fresh
> install — build from the repo per `README.md` / `INSTALL.md`.
> There is no newer release tarball yet. The body below is kept
> verbatim as the record of what v0.2.0 was.

Date: 2026-11-03. Branch: `main`, commit `1309e7b`.
Tag: `v0.2.0` (pushed, verified via `ls-remote`).

## One-click install (VPS, Ubuntu 22.04+/Debian 12 x86_64)

```bash
curl -LO https://github.com/filewrap/ParlayMusic/releases/download/v0.2.0/parlay-v0.2.0-linux-x86_64.tar.gz
tar -xzf parlay-v0.2.0-linux-x86_64.tar.gz
cd parlay-v0.2.0-linux-x86_64 && ./install.sh && ./start.sh
```

Stop / status any time: `./stop.sh`, `./status.sh`. Full manual guide:
[INSTALL.md](INSTALL.md).

## What was built

- `install.sh` — one-command setup: OS/arch gate, apt deps (curl, git,
  python3+venv, node, ffmpeg, unzip, openssl), release-tarball
  download+extract (SHA256SUMS verified) or `--from-repo` clone,
  `data/`/`logs/`/`run/` dirs, `.venv` + downloaders requirements,
  standalone `bin/yt-dlp`, slskd 0.26.2 fetched from upstream,
  `.env` mode 600 (interactive prompt, no-echo for token/hash, or env
  vars non-interactively; `PARLAY_TOKEN` auto-generated), validation
  (`BOT_TOKEN` digits:…, `API_ID` numeric). Idempotent — safe to
  re-run, never overwrites `.env`.
- `start.sh` / `stop.sh` / `status.sh` — native launcher, no Docker.
  13 services in dependency order (slskd opt-in → downloaders →
  netease → spotify → vc-go → vc-ts → vc-rust → audio-fx → share →
  queue → radio → gui → core last), `GET /healthz` wait up to 30 s
  each, logs `logs/<service>.log`, PIDs `run/<service>.pid`,
  double-start and missing-`BOT_TOKEN` refusal, non-zero exit naming
  failed services. Works from both the tarball layout and the repo
  layout. `stop.sh` kills by PID file + exact port-owner (no `pkill -f`).
- `scripts/package-release.sh` — release-mode builds of everything
  (core-cpp cmake Release, vc-go + netease-go, 5 Rust crates
  `--release`, JS/Python sources copied) and packs the tarball.
- `INSTALL.md` — manual VPS guide: prereqs, one-click + manual paths,
  systemd unit (`Type=oneshot` + `RemainAfterExit=yes`, because
  start.sh exits after spawning), ports table, logs, updates,
  troubleshooting.
- `VERSION` — `v0.2.0` (matches `contracts/ipc.json` 0.2.0).
- README one-click section at the top.

## Release artifact

- `release/parlay-v0.2.0-linux-x86_64.tar.gz` — 63,648,819 bytes (61M),
  66 tar entries (53 files + 13 dirs).
- SHA256:
  `4ca324c7c73ec5c9aff6e9184ddceb7342f60dd8365d49483a06dfb0d8b0811c`
  (sidecar `.sha256` next to the tarball; `SHA256SUMS` inside it,
  52/52 verified).
- Contents: `bin/` (8 binaries), `lib/libtdjson.so.1.8.69`
  (50,166,944 B, prebuilt — TDLib NOT rebuilt) +
  `lib/libntgcalls.so` (98,121,040 B, prebuilt),
  `services/{vc-ts,gui-js,radio-js,downloaders}/` (sources only),
  `install.sh`, `start.sh`, `stop.sh`, `status.sh`, `.env.example`,
  `README.md`, `INSTALL.md`, `VERSION`, `SHA256SUMS`.
  No `.env`, no tokens, no session files, no `refs/`, no build dirs,
  no venvs. slskd is NOT in the tarball — `install.sh` fetches it
  from upstream (pinned 0.26.2).
- Secret scan clean: 0 matches for the sandbox `.env`'s live
  `BOT_TOKEN` / `API_HASH` in the tree or the tarball; no `.env` /
  session files tracked in git.

## Verification (coordinator, clean-room, 2026-11-03)

Fresh dir `~/workspace/parlay-cleanroom` (NOT `/tmp` — the sandbox
`/tmp` is a 512 MB tmpfs, too small):

- Extract: `tar -xzf` exit 0; `sha256sum -c SHA256SUMS` → **52/52 OK**.
- Install, dummy creds
  (`BOT_TOKEN=123456:TEST API_ID=123 API_HASH=test OWNER_ID=1`,
  non-interactive): exit 0 — venv + requirements + yt-dlp 2026.10.26
  + slskd 0.26.2.0 fetched from upstream (live, through the proxy) +
  `.env` written mode 600. Idempotent re-run keeps `.env`.
- Start, dummy creds: **12/12 OK, exit 0** (slskd SKIPPED, opt-in).
  Core passes `/healthz` then exits on the dummy token's 401
  (expected core behaviour).
- Start, real token (sandbox `.env` values copied in, never printed):
  **12/12 OK**; `GET /api/now-playing` returns honest
  `{"error":"no playback state","ok":false}`; core holds the live
  long-poll — competing `getUpdates?timeout=30` got
  `409 Conflict: terminated by other getUpdates request`, and the
  core's own log shows `[poll] getUpdates error: HTTP 409 …
  retrying in 1s`. (One earlier getMe attempt timed out on the
  egress proxy — transient; a retry seconds later returned
  `{"ok":true,"username":"punlobot",…}`. The core's 15 s getMe
  timeout makes it exit on such flakes; on a VPS with stable
  egress this does not apply.)
- `status.sh`: 12/12 UP. `stop.sh`: all stopped, all ports free,
  0 stray processes.
- slskd enabled path (Worker A, dummy Soulseek creds): **13/13 OK**,
  slskd UP (HTTP 200 on `/`).

## GitHub Release status — ACTION NEEDED

The code + tag are pushed (`main` `1309e7b`, tag `v0.2.0`), but the
**GitHub Release itself is NOT created**: the GitHub MCP connector
exposes only `list_releases` / `get_release_by_tag` /
`get_latest_release` — there is NO `create_release` or asset-upload
tool in its live catalog (46 tools checked), and the connector's
OAuth token cannot be extracted for a direct REST call. This is a
connector limitation, not a Parlay problem.

**Fallback — create it in one command (needs `gh` authed once):**

```bash
gh release create v0.2.0 \
  release/parlay-v0.2.0-linux-x86_64.tar.gz \
  release/parlay-v0.2.0-linux-x86_64.tar.gz.sha256 \
  --title "Parlay v0.2.0 — one-click install/start" \
  --notes "One-click install/start. Prebuilt release tarball (no compilation on the VPS): install.sh + start.sh + stop.sh + status.sh. See INSTALL.md. Tarball SHA256: 4ca324c7c73ec5c9aff6e9184ddceb7342f60dd8365d49483a06dfb0d8b0811c"
```

Or via the web UI: https://github.com/filewrap/ParlayMusic/releases/new
→ choose tag `v0.2.0` → upload the two files above. After publishing,
re-test the one-click download URL
(`releases/download/v0.2.0/parlay-v0.2.0-linux-x86_64.tar.gz`) once —
it was untestable before the release exists.

Expected release URL once published:
https://github.com/filewrap/ParlayMusic/releases/tag/v0.2.0

## What remains

- Publish the GitHub Release (see above — one command).
- Live voice join still never happened (needs phone/QR or a
  pre-authorized session + open-network host); unchanged from Phase 6.
- `docker compose up` still never run (no Docker in the sandbox);
  the native one-click path is the verified route.
- Mongo aside; key-gated paths (Gemini, Spotify OAuth, Deezer ARL,
  NetEase MUSIC_U, Genius/Musixmatch, Lemonic, Soulseek account)
  stay env-gated.
- `OWNER_ID`/`SUDO_IDS` + `PARLAY_TOKEN` are still unset in the
  sandbox `.env` (the clean-room test auto-generated a token).
