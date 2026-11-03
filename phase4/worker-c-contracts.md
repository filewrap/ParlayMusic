# Phase 4 — Worker C (contracts, config values, docs)

Date: 2026-11-03. Base: main at `d36cdd7` (wiring-audit fixes pushed).
Scope: `contracts/*`, `COMMANDS.md`, `.gitignore`, minimal doc touch-ups
(`README.md`, `CURRENT-STATE.md`). Did NOT touch `.env`, did NOT push,
did NOT touch `services/core-cpp` or `services/backend-go` (other workers
own those — their concurrent edits were present in the working tree and
were left alone), did NOT start Phase 5, no stubs/mocks/placeholders.
Single source of truth: `FINAL-ARCHITECTURE.md` (locked, not edited).

## 1. Q5 — `contracts/ipc.json` retargeted to the unified backend

All 65 actions retargeted; action set identical (verified: same 65 keys,
same request/response shapes except the two `chat.settings` annotations
below; contract version stays `0.3.1`).

- **65/65 `service` fields** rewritten from deleted-service names to
  `backend-go:<package>`, mapped from the backend's actual `Register`
  calls (`services/backend-go/internal/*/Register` + `handlers.Register`):
  `handlers` 20 (all `vc.*`), `spotify` 11, `download` 11 (`download.*` 10
  + `mix.generate`), `search` 4, `netease` 3, `radio` 4, `library` 4
  (`library.*` 3 + `cache.clear`), `share` 2, `vote` 1 (`queue.vote`),
  `fx` 1, `settings` 1 (`chat.settings`), `gui` 2, `recognize` 1
  (`audio.recognize`). Note: the audit said "52 service fields" — the
  recount is 65 changed (56 named a deleted service or combination,
  9 were bare `backend-go` and gained the owning package).
- **65/65 `method` fields** rewritten from `POST|GET /v1/…` to
  `UDS <action>` (the envelope carries no method).
- **8 `from` fields** fixed where they named a dead mechanism or a
  deleted service file (provenance kept, framed as reference):
  `downloaders server.py` → dropped (IPC-4 provenance kept);
  `radio-js src/main.js` (×2) → openradiobot reference;
  `vc-ts src/main.js` (lyrics panel, panel callbacks, volume control)
  → reference; `dlopen'd ntgcalls` (×2, `vc.join`/`vc.mute`) →
  build-time-linked ntgcalls. All other `from` values are external-ref
  or inventory provenance and were left alone.
- Top-level `description` rewritten (UDS + JSON, socket path, no HTTP
  routes, no per-service endpoints); `transport.protocol`
  `HTTP/1.1 + JSON` → `UDS (Unix domain socket) + JSON — length-prefixed
  frames`; `auth_header: X-Parlay-Token` removed, replaced by an `auth`
  note (socket 0600 in a 0700 data dir, §7). `timeout_ms` 30000 kept.
- `notes`: the v0.3.0/v0.3.1 entries kept verbatim as dated history; a
  Phase 4 note appended recording the retarget. The undated
  "New actions are added here first, then in the service…" note →
  "then in the backend package".
- Two further code-grounded fixes in lines already being retargeted:
  `download.prefs` format list `mp3|aac|flac|opus|m4a|wav` →
  `mp3|m4a|flac|opus|ogg|wav` (matches `DownloadFormats` in
  `internal/download/prefs.go:36` — the contract listed `aac`, the
  validator accepts `ogg`, not `aac`); `chat.settings` /
  `download.prefs` `(POST only)` → `(set only)` (UDS has no method;
  a payload with key+value is a set, key-only is a get);
  `spotify.auth` note `POST action:callback` → `call spotify.auth
  with action:callback`.
- **Validation:** `python3 -m json.tool contracts/ipc.json` passes;
  programmatic check confirms 65 actions, set identical to the
  pre-edit file, version still `0.3.1`.

## 2. Q5 (docs) — `contracts/uds-protocol.md`, `contracts/routing-table.md`, `contracts/README.md`

- `uds-protocol.md`: "FROZEN parity reference — the 56 actions" →
  live contract, 65 actions; the §4 legacy-shim note corrected (the
  shim did NOT die in Wave 2 — `IpcClient::canonicalAction` still
  translates legacy names, because the dispatcher still posts them);
  §8 `GET-style` deadline class → `read-style (fast reads)`;
  `(download POSTs pass 120 s, …, GETs 10 s)` → `(download calls pass
  120 s, …, reads 10 s)`.
- `routing-table.md`: header `56 actions, frozen` → `65 actions`;
  A75/A76 `queue-rust service` → `backend-go:vote`; A93/A94
  `spotify-rust service` → `backend-go:spotify`; D6 `POST /v1/lyrics`
  → `UDS vc.lyrics, payload {chat_id}`; §D router note `vc-ts
  resolvers.js inlineMode exactly` → ports that parsing from the
  vc-ts reference; §H note `queue-rust`/`spotify-rust` → the
  `backend-go:*` packages; the `Phase 3` status legend rewritten to
  past tense (the backend actions exist since Phase 3; a row still
  marked `Phase 3` records remaining core wiring, an unrouted
  command, or an inline gap).
- `contracts/README.md` (found by the stale-wording sweep — it still
  described HTTP throughout): transport → UDS + socket path; health
  `GET /healthz` → the `ping` UDS action; the "core rule" rewritten
  (no engine fallback list exists — the unified backend is the only
  engine, a failed call fails honestly); "then in the service" →
  "then in the backend package"; the Core HTTP API section kept (the
  core's own `:8080` surface is intentional) with the "same fail-over"
  phrase fixed to "same data as the Telegram commands".
- `README.md` + `CURRENT-STATE.md`: "`ipc.json` … frozen as the parity
  reference" → "the live contract … retargeted to the unified backend
  in Phase 4" (one line each; no wholesale rewrite).

## 3. Q8 — `autoplay_lang` canonical default: `""`

**Decision: `""` (any language).** Evidence (code, not guesswork):
- The wire-visible default IS `""`: `settings.defaults()` overrides
  `DefaultChatSettings()` (`settings.go:99-100`), pinned by
  `settings_test.go:45`.
- The inventory agrees: `phase3/actions-inventory.md` §8.1 line 1936
  and §10-U3 propose `""` ("no value exists anywhere in-tree").
- `model.DefaultChatSettings()` returns `"en"` (`model.go:220`,
  pinned by `model_test.go:95`) but its comment claims these are "the
  legacy values the core's Dispatcher::chatSettings reads" — the core
  has NO `autoplay_lang` value anywhere (zero hits in `core-cpp`),
  so the `"en"` is ungrounded. No backend consumer of `AutoplayLang`
  exists yet (only `model.go` + `settings.go` reference it).
- **Docs now agree:** `ipc.json` `chat.settings` response annotates
  `autoplay_lang` as `string (default "" — any language)`.
- **Follow-up for the backend worker (NOT done — `backend-go` is not
  my scope):** one line in `model.go:220` (`"en"` → `""`) + update
  `model_test.go:95`. Until then the two defaults disagree in code.

## 4. Q9 — `play_mode` vocabulary: `"admins"`

**Decision: `"admins"` (with `"everyone"`).** Evidence:
- The wire contract validates `"everyone"` | `"admins"` only and
  rejects `"admin"` (`settings.go:164-172`, pinned by
  `settings_test.go:58-70`); the inventory §8.1 line 1930 specifies
  `"everyone"` / `"admins"`; `settings.go:12` doc comment agrees.
- The core's own settings-panel parser already accepts `"admins"`
  (and `"adminonly"`) as input (`dispatcher.cpp:4870`) and normalizes
  — only its *stored* spelling is `"admin"` (`:4878`, toggle `:7262`,
  gate `:1099` checks `== "admin"`). The core's stored `"admin"` is
  the outlier, and it is fatal the day the stores unify (Q7): the
  backend would reject the core's own stored value.
- No backend consumer of `PlayMode` exists yet (handlers/room never
  read it) — the vocabulary only matters for storage/validation today.
- **Docs now agree:** `ipc.json` `chat.settings` annotates
  `play_mode` as `string (everyone|admins; default everyone)`;
  `COMMANDS.md` header `(admin/everyone)` → `(admins/everyone)`.
  The `/playmode` usage `[admin|everyone]` is left as-is: it is the
  *input* spelling, and the core parser accepts `admins` too.
- **Follow-up for the core worker (NOT done — `core-cpp` is not my
  scope):** 3 lines in `dispatcher.cpp` — `:4878` write
  `"admin"` → `"admins"`, `:7262` toggle, `:1099` gate
  `== "admin"` → `== "admins"`.

## 5. Q14 — `bin/` gitignored

Added `bin/` to `.gitignore`. `git ls-files bin/` = 0 tracked files,
and no `bin/` directory exists in the tree, so nothing needed
untracking and no user data was touched. (`start.sh` builds the
backend into `$ROOT/bin/parlay-backend`; that output is now ignored.)

## 6. Q15 — `COMMANDS.md` code-grounded pass

Checked against `dispatcher.cpp` `registerCommands` (86 `add()` calls,
108 command names), the `aliases_` map (70), `channel_cmds_` (35),
every `on<X>` handler body, and the backend `Register` calls /
`vc.queue` / room code. **83 table rows + 13 "Not yet" bullets +
header + Callback + Inline sections checked; 42 replacements made.**

Who-column corrections (26 values — the systematic finding is that
`play_mode` gates `/play` only; `playAllowed` is called solely by
`onPlay`/`onPlayPlaylist`, while every other playback/queue command
uses `canManageVc`):
- `play_mode` → `admin`: `/pause`, `/resume`, `/skip`, `/next`,
  `/stop`, `/end`, `/loop`, `/seek`, `/seekback`, `/jump`, `/speed`,
  `/bass`, `/replay`, `/clear`, `/restore`, `/remove`, `/move`,
  `/shuffle`, `/autoplay-skip`, `/askip`, `/radio` (17 rows).
- `/download` `E` → `admin` (`onDownload` starts with `canManageVc`).
- `/autoleave` `admin` → `sudo` (route `Perm::Sudo`);
  `/maintenance` `sudo` → `owner` (route `Perm::Owner`);
  `/restart` `owner` → `sudo` (route `Perm::Sudo`, no in-handler
  owner check); `/listsudo` `owner` → `E` (route `Everyone`, no
  in-handler check — anyone may list sudoers); `/json` `sudo` → `E`
  (route `Everyone`, no check); `/userbot` `admin` → `E` (no gate);
  `/setrtmp` `admin` → `E (DM only)` (route `Everyone`,
  `Scope::Private`, no admin check); `/streamstatus` `admin` → `E`
  (no gate; `/stream` + `/streamstop` stay `admin`).
- `/logger` split out of the `/logs` row: `/logs` = log tail (sudo);
  `/logger [on|off]` = toggles log forwarding to the configured
  logger chat (sudo; on with no logger id answers honestly).

Implements corrections (8):
- `/join`: "core-cpp TDLib backend; needs TD_* + libntgcalls.so" →
  `vc.join` UDS; backend drives the JoinRequest handshake, core runs
  TDLib `joinVideoChat` on the assistant session.
- `/speed`, `/bass`: "applied for real on core-voice sessions
  (atempo re-attach / lowpass mix)" → **stored only, NOT applied** —
  the stream restarts at the current position at the old speed/bass;
  live filtering is the Phase 5 backend DSP (`onSpeed`/`onBass` reply
  this plainly). The old table text directly contradicted the file's
  own "Not yet" bullet, which was right.
- `/mute`: "core-owned sessions … engine-owned sessions honestly
  refuse" → `vc.mute` UDS (backend ntgcalls mute); no engine-owned
  sessions exist anymore (`muteVoice` is one UDS call).
- `/move`: "`vc.queue` move" → no `move` verb exists — the backend
  rejects it (`handlers/vc.go:363`, want `add|remove|clear|list`);
  the core implements move as list → remove → re-add.
- `/autoplay`: "`vc.autoplay` (backend-owned…)" → Store only (core
  `autoplay`/`ap_lang`/`ap_mood` chat fields); `onAutoplay` contains
  zero `vcCall`/`ipc_` calls — the backend room is never told.
- `/radio`: "`vc.radio` → backend radio package" → `radio.list` (no
  arg) / `radio.start` + `vc.play` of the stream URL (station);
  `/radio` never calls `vc.radio`.
- `/speedtest`: "Cloudflare `__down`/`__up`…" → answers honestly
  unavailable — the probes are gone (`onSpeedtest` replies "Speed
  test is not available yet — the network probes need a backend
  action that does not exist yet").

"Not yet" bullets corrected (8):
- Download cancel: "Downloads also run async now (`async` flag,
  `DownloadReady` event…)" → **false** — zero `async`/`DownloadReady`
  hits in core or backend; downloads are synchronous, the progress
  message is edited in place, the final snapshot is fetched after
  the call returns. Remaining step is wiring `cbDl` (Q4).
- Inline `lib`/`del`: "backed by the real library now" → **false** —
  they return empty results ("only if a future library backend
  returns tracks"); the library actions exist but the inline path
  does not call them. The Inline section's own text was right.
- Library delete: "no `library.delete` IPC action exists" → **false**
  — the action exists (OD-3); the core does not call it yet (Q4).
- Spotify companion surfaces: "the core only routes `spotify.dislike`"
  → **false** — `cbSpotify` routes `dislike` + `details` + `analyze`
  and renders the cards in place.
- `/settings` download prefs: "the contract's download actions take
  no such fields (needs a contract addition)" → **false** — the
  contract HAS `download.prefs` (OD-3); the core does not call it.
- Spotify `language`/`features`: "the core renders no `/details` card
  (spotify-rust owns §25)" → **false** — the core renders it
  (`cbSpotify` → `spotifyDetailsText`, backend-go:spotify data) and
  hides the nulls itself.
- `/play` from replied media: dropped the "(Phase 5)" label (new
  Phase 5 has not started) and corrected the mechanism to the
  `vc.play_file` UDS action.
- `/broadcast` errors file: dropped the "(Phase 6 Worker C)" label.

Header fixes: `c*` count `34` → `35` (counted 35 keys in
`channel_cmds_`, including `/caskip`); `play_mode (admin/everyone)` →
`(admins/everyone)`; "maintenance → sudo only" → "owner only";
"Every command dispatches to a real UDS call…" now excepts
`/speedtest` and retired `/engine`; added a `play_mode`-vs-admin-gate
explainer paragraph and a note listing the registered-but-unlisted
secondary names (`/lyric`, `/language`, `/thumb`, `/reload`,
`/rmsudo`, `/sudolist`, `/ev`, `/sh`, `/exec` + ~70 aliases).

Verified-correct and left alone (spot-checked, not exhaustive in
this report): `/play` flags (`-f`/`-v`), `/skip` resets loop
(`room.go:642-653`), `/loop` 0–10, `/queue`/`/pos`/`/player`
(9-stage `◉` bar, 5-row keyboard), `/search` (5/row, 10/page, 5 s
cooldown), `/download` picker/lossy/FX prompts, `/lyrics` (`lrc_path`),
`/share` (My Link button), `/auth` 25-cap, `/admincache` 600 s,
`/broadcast` 200 ms + `errors.txt`, `/eval` 60 s `sh -c`,
`/restart` exit 42 + confirm card, `/ping` card + `PING_IMG`,
`/status` zeros, `/help` group DM prompt, `/start spotify----` deep
link, all 18 callback domains, inline `np`/`s`/`l`/`help`/`lib`/`del`
modes, `last_chat:`/`inlinesearch:` stores, blacklisted/maintenance
inline silencing.

## 7. Q13 — NOT decided (flagged)

`VERSION` left at `v0.2.0` (verified: `cat VERSION` → `v0.2.0`).
No version was bumped anywhere; `ipc.json`'s contract version stays
`0.3.1`. The old v0.2.0 release asset in `release/` was not touched.

## 8. Verification before finishing

- `python3 -m json.tool contracts/ipc.json` → parses; 65 actions,
  set identical, version `0.3.1`.
- Stale-wording grep over `contracts/` + `COMMANDS.md` for
  `vc-go|vc-ts|vc-rust|downloaders|netease-go|queue-rust|radio-js|
  share-rust|spotify-rust|audio-fx-rust|HTTP/1.1|X-Parlay-Token|
  POST /v1|GET /v1|GET /healthz` → **0 live hits**. Remaining matches
  are deliberate: `(reference; … deleted in the Phase 3 unification)`
  provenance in 5 `from` fields + 1 routing-table note; the dated
  v0.3.0/v0.3.1 history notes in `ipc.json`; `GET /healthz` in
  `contracts/README.md` (the core's own `:8080` API, intentional)
  and its negation in `uds-protocol.md` ("no `/healthz` polling").
  `56 actions` → 0 hits outside `56 + 9 OD-3` / `56 contract + 9 OD-3`.
- `git ls-files bin/` → 0 tracked; `bin/` now in `.gitignore`.
- `.env` untouched (not in `git status`); nothing pushed.
- Files changed (mine only, 7): `.gitignore` (+1),
  `COMMANDS.md` (+80/−62), `CURRENT-STATE.md`, `README.md`,
  `contracts/ipc.json` (+301/−301 lines incl. 65 service + 65 method
  rewrites), `contracts/routing-table.md`, `contracts/uds-protocol.md`,
  `contracts/README.md`. (The working tree also carries other
  workers' concurrent `services/*` changes — not mine, not touched.)

## 9. Not resolved / left for others

1. **Backend worker:** `model.go:220` `AutoplayLang "en"` → `""` +
   `model_test.go:95` (Q8 one-liner; see §3).
2. **Core worker:** `dispatcher.cpp` play_mode `"admin"` → `"admins"`
   at `:4878` (write), `:7262` (toggle), `:1099` (gate) (Q9; see §4).
3. **Q4 owner:** routing-table rows A63 (`pl:dl:cancel`) and A81
   (`pl:del:yes`) still say "no `download.cancel` / `library.delete`
   action exists in `contracts/ipc.json`" — false since Phase 3; the
   rows need updating when Q4 (wire vs keep-unavailable) is decided.
   I left the row text alone because the wiring decision is Q4's.
4. **Coordinator/user:** `FINAL-ARCHITECTURE.md` §10 still anchors
   the parity target at **56 actions** ("51 actions at 1aa39cc →
   56 actions at … The parity target is 56 actions (§10)"). The
   live contract is 65 (56 + 9 OD-3). The locked doc was not edited;
   the 56→65 question from audit Q5 is still open.
5. **Unverified, left as-is in `COMMANDS.md`:** the "Share
   feedback/report dedup" and "Private-bot notice" bullets (no code
   evidence found either way) and the "TS / Rust extras" paragraph
   (aliases spot-checked only).
6. **Q13:** next release version still undecided; `VERSION` is
   `v0.2.0`.
