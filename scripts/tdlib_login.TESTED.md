# tdlib_login.py — offline test note

Script: `scripts/tdlib_login.py` (598 lines, stdlib + ctypes only).
Tested: 2026-11-03 ~10:20 IST, sandbox, against the real built
`phase4/tdlib-src/build/libtdjson.so.1.8.69` (TDLib 1.8.69).

## Defaults verified against the core (no mismatches)

- `PARLAY_DATA_DIR` defaults to `data` — matches `config.hpp:86`.
- `TD_SESSION_DIR` defaults to `<data_dir>/tdlib` — matches
  `config.cpp:93` (`envStr("TD_SESSION_DIR", c.data_dir + "/tdlib")`).
- `TD_API_ID` falls back to `API_ID`, `TD_API_HASH` falls back to
  `API_HASH` — matches `config.cpp:86-87` (`envInt("TD_API_ID", c.api_id)`
  / `envStr("TD_API_HASH", c.api_hash)`).
- `setTdlibParameters` payload matches `tdlib_client.cpp:256-270`
  (same directories, empty encryption key, `use_test_dc: false`,
  `application_version: "0.1.0"`; only `device_model` differs
  intentionally: `"Parlay tdlib_login"` vs the core's `"Parlay core-cpp"`).

## What was run (all with dummy creds, session dirs under /tmp)

1. `--help` — prints usage, exit 0.
2. `--phone=+10000000000`, `--code=12345`, `--password=secret` —
   each refused with `ERROR: --<flag> is refused: credentials must
   come from a prompt or an env var ...`, exit 2.
3. Phone flow: `TD_API_ID=12345 TD_API_HASH=0123456789abcdef...`
   `TD_PHONE=+10000000000 TD_SESSION_DIR=/tmp/tdlib-login-test/tdlib
   --timeout 45` — observed:
   - `Session dir: /tmp/tdlib-login-test/tdlib (mode 0o700)`
   - `Loaded libtdjson.so from .../phase4/tdlib-src/build/libtdjson.so.1.8.69`
   - `-> authorizationStateWaitTdlibParameters` (setTdlibParameters sent)
   - `-> authorizationStateWaitPhoneNumber` (phone number sent)
   - `ERROR: authorization timed out after 45s in state authorizationStateWaitPhoneNumber`,
     exit 1. Session dir contains real TDLib files (`db.sqlite`,
     `td.binlog`, `files/`), proving the real library is being driven.
4. QR flow: `TD_QR=1 ... --timeout 30` — observed
   `WaitPhoneNumber — requesting QR login ...`, then
   `ERROR: authorization timed out after 30s in state
   authorizationStateWaitPhoneNumber`, exit 1. The login link never
   arrives offline, as the script's docstring states.
5. Bug found and fixed in this commit: an explicit `--lib` / `TDJSON_LIB`
   path that failed to load was silently falling back to the next
   candidate, although `--lib` is documented as "overrides the candidate
   search". Now an explicit path is tried alone and fails loudly:
   `--lib /nonexistent/libtdjson.so` → `ERROR: TDJSON_LIB=/nonexistent/
   libtdjson.so: no such file`, exit 2, no fallback. Re-tested both ways:
   good explicit lib loads from that exact path and drives the auth
   states; default path (no `--lib`) still loads the repo-build lib.

## Why the timeouts are the honest outcome

The sandbox has proxy-only egress and no direct TLS to Telegram DCs,
so the `setAuthenticationPhoneNumber` / QR requests can never get a
reply here. The script waits for its `--timeout`, then fails with the
exact state and exit 1 — no fake success, no hang (every run finished
within its timeout + a few seconds).

## Remaining proof — NOT done here

A REAL login can only be proven by the user with their own phone, on a
host that can reach Telegram: run `python3 scripts/tdlib_login.py`,
enter the phone number, the code Telegram sends, and the 2FA password
if set (or `--qr` and scan the printed link). Success is the
`Logged in as: <name> (id <id>)` line plus `getMe` succeeding; the
session dir (`data/tdlib` by default, mode 0700) is then reused by
parlay-core with no credentials needed.
