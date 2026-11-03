#!/usr/bin/env python3
"""tdlib_login.py — fresh TDLib login for Parlay's core-cpp voice backend.

Logs a Telegram *user* account into TDLib and saves the session to the
directory the core reads (TD_SESSION_DIR). Run it once on the host that
runs parlay-core; after it prints the logged-in account, start the core
and it will reuse the session with no phone/code/password needed.

    python3 scripts/tdlib_login.py
    python3 scripts/tdlib_login.py --qr          # QR login instead of phone
    TD_PHONE=+15551234567 TD_CODE=12345 python3 scripts/tdlib_login.py

Credentials come from interactive prompts or the TD_* env vars only.
This script takes NO credentials as command-line arguments: --phone /
--code / --password are refused outright (credentials in argv leak into
shell history and process listings). The phone number, code, and 2FA
password are NEVER written to disk or to any log; the session directory
is the only artifact, created with mode 0700.

Defaults (verified against services/core-cpp/src/config.cpp and
config.hpp — the core and this script agree exactly):
  * PARLAY_DATA_DIR defaults to "data"
  * TD_SESSION_DIR  defaults to <PARLAY_DATA_DIR>/tdlib  (so "data/tdlib")
  * TD_API_ID falls back to API_ID; TD_API_HASH falls back to API_HASH.

libtdjson.so is located in this order: TDJSON_LIB, <repo root>/lib/
(release layout: lib/libtdjson.so.1.8.69), phase4/tdlib-src/build/
(repo layout), /usr/local/lib/libtdjson.so, /usr/lib/libtdjson.so.
An explicit TDJSON_LIB (or --lib) is an override: it is tried alone
and a bad path exits loudly instead of silently falling back to
another library. If none loads the script exits loudly with the exact
reason.

IMPORTANT — string sessions cannot be imported into TDLib:
  A Pyrogram/Telethon *string session* CANNOT be imported into TDLib.
  It is a different session format and TDLib exposes no import API at
  all. This script therefore does a FRESH TDLib login (phone + code +
  optional 2FA password, or QR scan). If you want to keep using a
  Pyrogram string session, use the Python/Pyrogram+PyTgCalls assistant
  path instead — that is the path that accepts string sessions. This
  script does not pretend otherwise.

QR login: with --qr (or TD_QR=1) the script sends
requestQrCodeAuthentication and prints the login link TDLib returns
(authorizationStateWaitOtherDeviceConfirmation). Scan it with Telegram
on another device. The link only arrives after a live round-trip to a
Telegram DC; offline it stays empty and the script waits for it.

A REAL login can only be proven by the user with their own phone — this
script run offline proves the auth state machine is driven correctly
and that bad credentials fail with TDLib's exact error, not that a
live login succeeds.

Requires: Python 3.8+ (stdlib only) and a built libtdjson.so (1.8.69).
"""

import argparse
import ctypes
import getpass
import json
import os
import sys
import time

# ---------------------------------------------------------------------------
# Defaults — must match services/core-cpp/src/config.cpp exactly.
#   Config::fromEnv: data_dir = PARLAY_DATA_DIR or "data"
#                    td_session_dir = TD_SESSION_DIR or data_dir + "/tdlib"
# ---------------------------------------------------------------------------
DEFAULT_DATA_DIR = "data"


def default_session_dir():
    data_dir = os.environ.get("PARLAY_DATA_DIR", DEFAULT_DATA_DIR)
    return os.path.join(data_dir, "tdlib")


def repo_root():
    # scripts/tdlib_login.py -> <root>/scripts -> <root>
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def lib_candidates():
    root = repo_root()
    cands = []
    env_lib = os.environ.get("TDJSON_LIB")
    if env_lib:
        cands.append(("TDJSON_LIB", env_lib))
    cands.append(("release lib/", os.path.join(root, "lib", "libtdjson.so")))
    cands.append(("release lib/", os.path.join(root, "lib", "libtdjson.so.1.8.69")))
    cands.append(("repo build",
                  os.path.join(root, "phase4", "tdlib-src", "build",
                               "libtdjson.so.1.8.69")))
    cands.append(("repo build",
                  os.path.join(root, "phase4", "tdlib-src", "build",
                               "libtdjson.so")))
    cands.append(("system", "/usr/local/lib/libtdjson.so"))
    cands.append(("system", "/usr/lib/libtdjson.so"))
    return cands


def _bind_json_symbols(lib):
    """Bind the td_json_client_* symbols. Returns an error string or None."""
    try:
        lib.td_json_client_create.restype = ctypes.c_void_p
        lib.td_json_client_create.argtypes = []
        lib.td_json_client_send.restype = None
        lib.td_json_client_send.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.td_json_client_receive.restype = ctypes.c_char_p
        lib.td_json_client_receive.argtypes = [ctypes.c_void_p, ctypes.c_double]
        lib.td_json_client_execute.restype = ctypes.c_char_p
        lib.td_json_client_execute.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.td_json_client_destroy.restype = None
        lib.td_json_client_destroy.argtypes = [ctypes.c_void_p]
        # Optional global: td_set_log_verbosity_level(int).
        try:
            lib.td_set_log_verbosity_level.restype = None
            lib.td_set_log_verbosity_level.argtypes = [ctypes.c_int]
        except AttributeError:
            pass
    except AttributeError as e:
        return "missing td_json_client_* symbol: %s" % e
    return None


def load_libtdjson():
    """Load libtdjson.so.

    An explicit path (TDJSON_LIB or --lib) is an override: it is tried
    alone and a failure exits loudly instead of silently falling back
    to a different library. Without one, the first candidate that
    loads wins. Returns (lib, path). Raises SystemExit with the exact
    reason if nothing loads.
    """
    explicit = os.environ.get("TDJSON_LIB")
    if explicit:
        if not os.path.exists(explicit):
            sys.stderr.write("ERROR: TDJSON_LIB=%s: no such file\n"
                             % explicit)
            raise SystemExit(2)
        try:
            lib = ctypes.CDLL(explicit)
        except OSError as e:
            sys.stderr.write("ERROR: TDJSON_LIB=%s: failed to load: %s\n"
                             % (explicit, e))
            raise SystemExit(2)
        err = _bind_json_symbols(lib)
        if err:
            sys.stderr.write("ERROR: %s: %s\n" % (explicit, err))
            raise SystemExit(2)
        return lib, explicit
    failures = []
    for _label, path in lib_candidates():
        if not os.path.exists(path):
            failures.append("%s: no such file" % path)
            continue
        try:
            lib = ctypes.CDLL(path)
        except OSError as e:
            failures.append("%s: failed to load: %s" % (path, e))
            continue
        # Resolve the JSON-interface symbols (td/telegram/td_json_client.h).
        err = _bind_json_symbols(lib)
        if err:
            failures.append("%s: %s" % (path, err))
            continue
        return lib, path
    sys.stderr.write("ERROR: could not load libtdjson.so from any candidate:\n")
    for f in failures:
        sys.stderr.write("  - %s\n" % f)
    sys.stderr.write(
        "Build TDLib (phase4/tdlib-src) or set TDJSON_LIB to the .so path.\n")
    raise SystemExit(2)


class TdClient:
    """Minimal TDLib JSON client over ctypes (same shapes as tdlib_client.cpp)."""

    def __init__(self, lib):
        self.lib = lib
        self.client = lib.td_json_client_create()
        if not self.client:
            raise RuntimeError("td_json_client_create() returned null")
        self._extra = 0

    def set_log_verbosity(self, level):
        fn = getattr(self.lib, "td_set_log_verbosity_level", None)
        if fn is not None:
            fn(level)
            return
        self.execute({"@type": "setLogVerbosityLevel",
                      "new_verbosity_level": level})

    def execute(self, req):
        raw = self.lib.td_json_client_execute(
            self.client, json.dumps(req).encode("utf-8"))
        if not raw:
            return {}
        try:
            return json.loads(raw.decode("utf-8", "replace"))
        except ValueError:
            return {}

    def send(self, req):
        self.lib.td_json_client_send(
            self.client, json.dumps(req).encode("utf-8"))

    def receive(self, timeout):
        raw = self.lib.td_json_client_receive(self.client, timeout)
        if not raw:
            return None
        try:
            return json.loads(raw.decode("utf-8", "replace"))
        except ValueError:
            return None

    def call(self, req, timeout=30.0):
        """Send a request and wait for the response with the matching @extra."""
        self._extra += 1
        extra = "tdlib-login-%d" % self._extra
        req = dict(req)
        req["@extra"] = extra
        self.send(req)
        deadline = time.time() + timeout
        while time.time() < deadline:
            msg = self.receive(min(1.0, max(0.0, deadline - time.time())))
            if msg is None:
                continue
            if msg.get("@extra") == extra:
                return msg
            # Anything else is an update; stash it for the auth loop.
            self._pending.append(msg)
        return {"@type": "error", "code": 0,
                "message": "timeout waiting for response to %s"
                % req.get("@type")}

    def destroy(self):
        if self.client:
            self.lib.td_json_client_destroy(self.client)
            self.client = None

    _pending = []


# ---------------------------------------------------------------------------
# Prompts. Values are held in memory only and never written anywhere.
# ---------------------------------------------------------------------------

def prompt_phone():
    while True:
        phone = input("Phone number (international format, e.g. +15551234567): ").strip()
        if phone:
            return phone
        print("Phone number is required.")


def prompt_code():
    while True:
        code = getpass.getpass("Login code sent to Telegram: ").strip()
        if code:
            return code
        print("Code is required.")


def prompt_password():
    while True:
        pw = getpass.getpass("Two-step verification (2FA) password: ")
        if pw:
            return pw
        print("Password is required.")


def prompt_api_id():
    while True:
        raw = input("Telegram API ID (from https://my.telegram.org): ").strip()
        try:
            api_id = int(raw)
        except ValueError:
            print("API ID must be a number.")
            continue
        if api_id > 0:
            return api_id
        print("API ID must be a positive number.")


def prompt_api_hash():
    while True:
        h = getpass.getpass("Telegram API hash (from https://my.telegram.org): ").strip()
        if h:
            return h
        print("API hash is required.")


# ---------------------------------------------------------------------------
# Auth state machine (td_api.tl authorizationState*), mirroring
# TdLibClient::handleAuthState in services/core-cpp/src/tdlib_client.cpp.
# ---------------------------------------------------------------------------

class Login:
    def __init__(self, client, api_id, api_hash, session_dir, qr,
                 env):
        self.c = client
        self.api_id = api_id
        self.api_hash = api_hash
        self.session_dir = session_dir
        self.qr = qr
        self.env = env
        self.state = ""
        self.other_device_link = ""
        self._handled = set()

    def drive_state(self, state):
        """Drive one auth state once. Returns True to keep pumping."""
        if state in self._handled:
            return True
        self._handled.add(state)
        c = self.c

        if state == "authorizationStateWaitTdlibParameters":
            print("Auth state: WaitTdlibParameters — sending parameters "
                  "(session dir: %s)" % self.session_dir)
            c.send({
                "@type": "setTdlibParameters",
                "use_test_dc": False,
                "database_directory": self.session_dir,
                "files_directory": os.path.join(self.session_dir, "files"),
                "database_encryption_key": "",
                "use_file_database": True,
                "use_chat_info_database": True,
                "use_message_database": True,
                "use_secret_chats": False,
                "api_id": self.api_id,
                "api_hash": self.api_hash,
                "system_language_code": "en",
                "device_model": "Parlay tdlib_login",
                "system_version": "Linux",
                "application_version": "0.1.0",
            })
            return True

        if state == "authorizationStateWaitPhoneNumber":
            if self.qr:
                print("Auth state: WaitPhoneNumber — requesting QR login ...")
                c.send({"@type": "requestQrCodeAuthentication",
                        "other_user_ids": []})
                return True
            phone = self.env.get("TD_PHONE") or prompt_phone()
            print("Auth state: WaitPhoneNumber — sending phone number ...")
            c.send({
                "@type": "setAuthenticationPhoneNumber",
                "phone_number": phone,
                "settings": {
                    "@type": "phoneNumberAuthenticationSettings",
                    "allow_flash_call": False,
                    "allow_missed_call": False,
                    "is_current_phone_number": False,
                    "has_unknown_phone_number": False,
                    "allow_sms_retriever_api": False,
                    "firebase_authentication_settings": None,
                    "authentication_tokens": [],
                },
            })
            return True

        if state == "authorizationStateWaitOtherDeviceConfirmation":
            if self.other_device_link:
                print("Auth state: WaitOtherDeviceConfirmation")
                print("Scan this link with Telegram on another device:")
                print("  %s" % self.other_device_link)
            else:
                print("Auth state: WaitOtherDeviceConfirmation — "
                      "waiting for the login link ...")
            return True

        if state == "authorizationStateWaitEmailAddress":
            email = self.env.get("TD_EMAIL") or input(
                "Email address for this Telegram account: ").strip()
            print("Auth state: WaitEmailAddress — sending email address ...")
            c.send({"@type": "setAuthenticationEmailAddress",
                    "email_address": email})
            return True

        if state == "authorizationStateWaitEmailCode":
            code = self.env.get("TD_EMAIL_CODE") or getpass.getpass(
                "Code sent to the email address: ").strip()
            print("Auth state: WaitEmailCode — checking email code ...")
            c.send({"@type": "checkAuthenticationEmailCode",
                    "code": {"@type": "emailAddressAuthenticationCode",
                             "code": code}})
            return True

        if state == "authorizationStateWaitCode":
            code = self.env.get("TD_CODE") or prompt_code()
            print("Auth state: WaitCode — checking login code ...")
            c.send({"@type": "checkAuthenticationCode", "code": code})
            return True

        if state == "authorizationStateWaitPassword":
            pw = self.env.get("TD_PASSWORD") or prompt_password()
            print("Auth state: WaitPassword — checking 2FA password ...")
            c.send({"@type": "checkAuthenticationPassword",
                    "password": pw})
            return True

        if state == "authorizationStateWaitRegistration":
            first = self.env.get("TD_FIRST_NAME") or input(
                "First name for this new account: ").strip()
            last = self.env.get("TD_LAST_NAME", "")
            print("Auth state: WaitRegistration — registering user ...")
            c.send({"@type": "registerUser",
                    "first_name": first,
                    "last_name": last,
                    "disable_notification": False})
            return True

        if state == "authorizationStateReady":
            return True

        if state in ("authorizationStateClosed",
                     "authorizationStateClosing",
                     "authorizationStateLoggingOut"):
            raise RuntimeError("TDLib is %s" % state)

        # authorizationStateWaitPremiumPurchase and anything unknown:
        # cannot be driven headless — fail honestly.
        raise RuntimeError(
            "unsupported authorization state %s ("
            "authorizationStateWaitPremiumPurchase is an in-store purchase "
            "and cannot be driven headless; log in once with a full client "
            "and reuse TD_SESSION_DIR)" % state)

    def run(self, timeout=600.0):
        """Pump updates until Ready or timeout. Returns when authorized."""
        c = self.c
        c.send({"@type": "getAuthorizationState"})
        deadline = time.time() + timeout
        while time.time() < deadline:
            # Drain anything stashed by call() first.
            msg = c._pending.pop(0) if c._pending else c.receive(1.0)
            if msg is None:
                continue
            mtype = msg.get("@type", "")
            if mtype == "updateAuthorizationState":
                st = msg.get("authorization_state", {})
                new_state = st.get("@type", "")
                if new_state != self.state:
                    self.state = new_state
                    print("-> %s" % new_state)
                    if new_state == "authorizationStateWaitOtherDeviceConfirmation":
                        link = st.get("link", "")
                        if link:
                            self.other_device_link = link
                    if new_state == "authorizationStateReady":
                        return True
                    # A state change resets the wait budget (prompts block).
                    deadline = time.time() + timeout
                    self.drive_state(new_state)
                continue
            if mtype == "error":
                raise RuntimeError("TDLib error %s: %s"
                                   % (msg.get("code"), msg.get("message")))
            if mtype == "authorizationStateReady":
                self.state = mtype
                return True
            # Other updates (connection state, etc.): ignore, keep pumping.
        raise RuntimeError("authorization timed out after %ds in state %s"
                           % (timeout, self.state or "(none seen)"))


def prepare_session_dir(session_dir):
    """Create the session dir with mode 0700 (or fix the mode if present)."""
    if os.path.isdir(session_dir):
        st = os.stat(session_dir)
        if st.st_mode & 0o777 != 0o700:
            os.chmod(session_dir, 0o700)
            print("Session dir %s: fixed permissions to 0700" % session_dir)
    else:
        os.makedirs(session_dir, mode=0o700, exist_ok=True)
        os.chmod(session_dir, 0o700)
    st = os.stat(session_dir)
    mode = oct(st.st_mode & 0o777)
    print("Session dir: %s (mode %s)" % (session_dir, mode))
    if mode != "0o700":
        raise RuntimeError("could not set session dir mode to 0700 "
                           "(got %s)" % mode)


def main(argv=None):
    # Refuse credential flags before argparse sees them: credentials in
    # argv leak into shell history and process listings.
    forbidden = ("--phone", "--code", "--password", "--api-hash",
                 "--api_hash")
    for arg in (argv if argv is not None else sys.argv[1:]):
        name = arg.split("=", 1)[0]
        if name in forbidden:
            sys.stderr.write(
                "ERROR: %s is refused: credentials must come from a prompt "
                "or an env var (TD_PHONE / TD_CODE / TD_PASSWORD), never "
                "from a command-line argument.\n" % name)
            return 2

    p = argparse.ArgumentParser(
        description="Fresh TDLib login for Parlay (saves the session to "
                    "TD_SESSION_DIR for parlay-core to reuse).")
    p.add_argument("--session-dir",
                   help="session directory (default: $TD_SESSION_DIR or "
                        "<$PARLAY_DATA_DIR>/tdlib, PARLAY_DATA_DIR "
                        "defaults to 'data')")
    p.add_argument("--qr", action="store_true",
                   help="QR login instead of phone+code (also TD_QR=1); "
                        "prints the login link to scan on another device")
    p.add_argument("--timeout", type=float, default=600.0,
                   help="seconds to wait for each auth step "
                        "(default: 600)")
    p.add_argument("--lib",
                   help="explicit path to libtdjson.so (overrides the "
                        "candidate search; same as TDJSON_LIB)")
    args = p.parse_args(argv)

    if args.lib:
        os.environ["TDJSON_LIB"] = args.lib

    # --- credentials: env first, prompt if missing (never argv) --------
    api_id_raw = os.environ.get("TD_API_ID") or os.environ.get("API_ID")
    api_hash = os.environ.get("TD_API_HASH") or os.environ.get("API_HASH")
    try:
        api_id = int(api_id_raw) if api_id_raw else 0
    except ValueError:
        api_id = 0
    if not api_id:
        print("TD_API_ID / API_ID is not set.")
        api_id = prompt_api_id()
    if not api_hash:
        print("TD_API_HASH / API_HASH is not set.")
        api_hash = prompt_api_hash()

    session_dir = (args.session_dir
                   or os.environ.get("TD_SESSION_DIR")
                   or default_session_dir())
    session_dir = os.path.abspath(session_dir)
    qr = args.qr or os.environ.get("TD_QR", "") in ("1", "true", "yes")

    try:
        prepare_session_dir(session_dir)
    except (OSError, RuntimeError) as e:
        sys.stderr.write("ERROR: session dir: %s\n" % e)
        return 2

    try:
        lib, lib_path = load_libtdjson()
    except SystemExit as e:
        return int(e.code or 2)
    print("Loaded libtdjson.so from %s" % lib_path)

    client = TdClient(lib)
    try:
        client.set_log_verbosity(1)
        login = Login(client, api_id, api_hash, session_dir, qr,
                      env=os.environ)
        try:
            login.run(timeout=args.timeout)
        except RuntimeError as e:
            sys.stderr.write("ERROR: %s\n" % e)
            return 1

        # Authorized — verify with getMe.
        me = client.call({"@type": "getMe"}, timeout=30.0)
        if me.get("@type") == "error":
            sys.stderr.write("ERROR: getMe failed %s: %s\n"
                             % (me.get("code"), me.get("message")))
            return 1
        username = ""
        for u in me.get("usernames") or []:
            if u.get("is_active"):
                username = u.get("username", "")
                break
        print("Logged in as: %s %s (id %s)%s"
              % (me.get("first_name", ""),
                 me.get("last_name", ""),
                 me.get("id"),
                 (" @" + username) if username else ""))
        print("Session saved to %s — start parlay-core and it will reuse it."
              % session_dir)
        return 0
    finally:
        try:
            client.send({"@type": "close"})
            # Brief drain so TDLib flushes its databases.
            for _ in range(20):
                if client.receive(0.1) is None:
                    break
        except Exception:
            pass
        client.destroy()


if __name__ == "__main__":
    sys.exit(main())
