"""HTTP IPC server — stdlib only (http.server.ThreadingHTTPServer).

Routes (contracts/ipc.json, transport: HTTP/1.1 + JSON):
  GET  /healthz                        -> {ok, service}
  POST /v1/download/youtube           {query, requested_by}
  POST /v1/download/spotify           {url, requested_by}
  POST /v1/download/spotify-playlist  {url, mode: zip|files, requested_by}
  POST /v1/download/deezer            {query|url, requested_by}
  POST /v1/download/tiktok            {url, requested_by}
  POST /v1/download/navidrome         {query|url, requested_by}
  POST /v1/search                     {query, source, limit}

Envelopes: success {ok:true, data:{...}}; error {ok:false,
error:{code, message}}. Auth: X-Parlay-Token checked on every POST
when $PARLAY_TOKEN is set; /healthz is unauthenticated.
"""
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Allow direct execution (`python src/server.py`) as well as module use
# (`python -m src.main`): relative imports need the package parent on path.
if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from src import providers  # noqa: E402
    from src.util import ApiError, bad_request, err_envelope, ok_envelope  # noqa: E402
else:
    from . import providers  # noqa: E402
    from .util import ApiError, bad_request, err_envelope, ok_envelope  # noqa: E402

SERVICE_NAME = "downloaders"

ROUTES = {
    ("POST", "/v1/download/youtube"): providers.youtube_download,
    ("POST", "/v1/download/spotify"): providers.spotify_download,
    ("POST", "/v1/download/spotify-playlist"): providers.spotify_playlist_download,
    ("POST", "/v1/search"): providers.search,
    ("POST", "/v1/download/deezer"): providers.PROVIDERS["deezer"]["download"],
    ("POST", "/v1/download/tiktok"): providers.PROVIDERS["tiktok"]["download"],
    ("POST", "/v1/download/navidrome"): providers.PROVIDERS["navidrome"]["download"],
}


class Config:
    def __init__(self):
        self.port = int(os.environ.get("PORT", "8010"))
        self.token = os.environ.get("PARLAY_TOKEN") or None
        self.dl_dir = os.environ.get("PARLAY_DL_DIR") or os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            "downloads")
        os.makedirs(self.dl_dir, exist_ok=True)


def make_handler(config):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):  # keep stdout clean
            pass

        def _send(self, status, payload):
            body = json.dumps(payload).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _send_error(self, err):
            self._send(err.status, err_envelope(err))

        def _check_auth(self):
            if config.token is None:
                return
            if self.headers.get("X-Parlay-Token") != config.token:
                raise ApiError("unauthorized",
                               "missing or wrong X-Parlay-Token", 401)

        def _read_json(self):
            length = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(length) if length else b""
            if not raw:
                return {}
            try:
                data = json.loads(raw.decode("utf-8"))
            except (json.JSONDecodeError, UnicodeDecodeError):
                raise bad_request("request body is not valid JSON")
            if not isinstance(data, dict):
                raise bad_request("request body must be a JSON object")
            return data

        def do_GET(self):
            if self.path == "/healthz":
                self._send(200, {"ok": True, "service": SERVICE_NAME})
                return
            self._send_error(ApiError("not_found",
                                      "no such route: GET %s" % self.path, 404))

        def do_POST(self):
            try:
                self._check_auth()
                handler = ROUTES.get(("POST", self.path))
                if handler is None:
                    raise ApiError("not_found",
                                   "no such route: POST %s" % self.path, 404)
                body = self._read_json()
                body["_dl_dir"] = config.dl_dir
                data = handler(body)
                self._send(200, ok_envelope(data))
            except ApiError as err:
                self._send_error(err)
            except Exception as exc:  # never leak a traceback to the caller
                self._send_error(ApiError("internal", str(exc), 500))

    return Handler


def serve(config=None):
    config = config or Config()
    server = ThreadingHTTPServer(("0.0.0.0", config.port),
                                 make_handler(config))
    print("parlay downloaders listening on :%d (dl_dir=%s, auth=%s)"
          % (config.port, config.dl_dir,
             "X-Parlay-Token" if config.token else "off"))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    # Direct execution (`python src/server.py`) serves, like `python -m src.main`.
    serve()
