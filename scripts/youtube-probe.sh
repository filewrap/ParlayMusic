#!/usr/bin/env bash
# youtube-probe.sh — prove, per approach, whether YouTube audio actually plays.
#
# Tests the three approaches the user ordered, IN ORDER, on the SAME video,
# and verifies REAL BYTES (not just "got a URL"): every stage fetches the
# first 64 KiB of the stream URL and reports the HTTP status + byte count.
# Nothing downloads a full file. No cookies are used unless Approach 2
# is explicitly given a cookie file.
#
#   ./scripts/youtube-probe.sh [VIDEO_URL_OR_ID] [COOKIES_FILE]
#
# Defaults: video https://www.youtube.com/watch?v=dQw4w9WgXcQ
# (public, standard test video). Cookies file: $2, else $YTDLP_COOKIEFILE,
# else the same locations the daemon checks (/root/cookies.txt,
# ~/cookies.txt, data/cookies.txt, cookies.txt). If no cookie file is
# found, Approach 2 is SKIPPED with a clear message — run it on the VPS
# where the cookies file lives.
#
# Approach 1 — direct cookieless playback (everything except cookie file)
# Approach 2 — with cookies file
# Approach 3 — API-based only (NubCoders /info)
#
# Env:
#   YTDLP_BIN        yt-dlp command (default: repo venv python -m yt_dlp,
#                    else system yt-dlp, else python3 -m yt_dlp)
#   NUBCODERS_TOKEN  API token for Approach 3 (default: the built-in
#                    default from services/backend-go/internal/resolver/nubcoders.go)
#   PROBE_RANGE      bytes to fetch per URL check (default 65535)
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VIDEO="${1:-https://www.youtube.com/watch?v=dQw4w9WgXcQ}"
COOKIE_ARG="${2:-${YTDLP_COOKIEFILE:-}}"
RANGE="${PROBE_RANGE:-65535}"

# ---------- tooling ----------
if [ -n "${YTDLP_BIN:-}" ]; then
    YTDLP="$YTDLP_BIN"
else
    # Prefer a candidate that actually RUNS: the venv's yt-dlp shim has a
    # stale shebang (points at services/downloaders/.venv, gone) and dies
    # with exit 127, so each candidate is verified with --version first.
    YTDLP=""
    for cand in "$ROOT/scripts/yt-dlp-venv/bin/yt-dlp" "$ROOT/scripts/yt-dlp-venv/bin/python -m yt_dlp" "$(command -v yt-dlp || true)" "python3 -m yt_dlp"; do
        [ -n "$cand" ] || continue
        if $cand --version >/dev/null 2>&1; then YTDLP="$cand"; break; fi
    done
    [ -n "$YTDLP" ] || YTDLP="python3 -m yt_dlp"
fi
PY="$(command -v python3)"
TOKEN="${NUBCODERS_TOKEN:-}"
if [ -z "$TOKEN" ] && [ -f "$ROOT/services/backend-go/internal/resolver/nubcoders.go" ]; then
    TOKEN="$($PY -c "import re;print(re.search(r'nubCodersToken = \"([^\"]+)\"', open('$ROOT/services/backend-go/internal/resolver/nubcoders.go').read()).group(1))" 2>/dev/null || true)"
fi

find_cookie() {
    [ -n "$COOKIE_ARG" ] && [ -f "$COOKIE_ARG" ] && { echo "$COOKIE_ARG"; return; }
    for c in /root/cookies.txt "$HOME/cookies.txt" "$ROOT/data/cookies.txt" "$ROOT/cookies.txt"; do
        [ -f "$c" ] && { echo "$c"; return; }
    done
    echo ""
}
COOKIE_FILE="$(find_cookie)"

ms_now() { date +%s%3N; }
elapsed() { echo $(( $2 - $1 )); }

# fetch_bytes <url> -> prints "<http_code> <bytes>" for the first $RANGE bytes.
# -L follows googlevideo 302 redirects; a Range request answers 206,
# which is success (200 is also accepted by the verdict logic).
fetch_bytes() {
    local url="$1"
    curl -s -L -o /dev/null -r "0-$RANGE" -w "%{http_code} %{size_download}" --max-time 30 "$url" 2>/dev/null || echo "000 0"
}

# http_ok: 200 (full) and 206 (Range) both mean bytes flowed.
http_ok() { [ "$1" = "200" ] || [ "$1" = "206" ]; }

# jget <json> <python-expr> -> value or empty
jget() { echo "$1" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print($2)" 2>/dev/null || echo ""; }

echo "=== youtube-probe ==="
echo "video: $VIDEO"
echo "yt-dlp: $($YTDLP --version 2>/dev/null || echo MISSING)"
echo "js runtimes: deno=$(command -v deno >/dev/null && echo yes || echo no) node=$(command -v node >/dev/null && node --version || echo no)"
echo "egress ip: $(curl -s --max-time 10 https://ifconfig.me 2>/dev/null || echo unknown)"
echo "cookies file: ${COOKIE_FILE:-NONE FOUND (Approach 2 will skip)}"
echo "nubcoders token: $([ -n "$TOKEN" ] && echo set || echo MISSING)"
echo ""

RESULTS=""

record() { # name time_ms url_ok http_code bytes verdict
    printf "%-28s %7s ms  url=%-3s http=%-3s bytes=%-7s %s\n" "$1" "$2" "$3" "$4" "$5" "$6"
    RESULTS="$RESULTS\n$1|$2|$3|$4|$5|$6"
}

# ---------- Approach 1: cookieless ----------
echo "--- Approach 1: direct cookieless playback (no cookie file anywhere) ---"

# 1a. yt-dlp defaults, no cookies
s=$(ms_now); URL=$($YTDLP --no-cookies -f bestaudio --get-url "$VIDEO" 2>/dev/null | head -n 1); e=$(elapsed "$s" "$(ms_now)")
if [ -n "$URL" ]; then read -r CODE BYTES <<<"$(fetch_bytes "$URL")"; else CODE="---"; BYTES=0; fi
http_ok "$CODE" && V="WORKS" || V="FAIL($CODE)"
record "1a ytdlp defaults" "$e" "$([ -n "$URL" ] && echo yes || echo no)" "$CODE" "$BYTES" "$V"

# 1b. yt-dlp tv client, no cookies (guide: no PO token, DRM risk without cookies)
s=$(ms_now); URL=$($YTDLP --no-cookies --extractor-args "youtube:player_client=tv" -f bestaudio --get-url "$VIDEO" 2>/dev/null | head -n 1); e=$(elapsed "$s" "$(ms_now)")
if [ -n "$URL" ]; then read -r CODE BYTES <<<"$(fetch_bytes "$URL")"; else CODE="---"; BYTES=0; fi
http_ok "$CODE" && V="WORKS" || V="FAIL($CODE)"
record "1b ytdlp tv" "$e" "$([ -n "$URL" ] && echo yes || echo no)" "$CODE" "$BYTES" "$V"

# 1c. yt-dlp visionos+web, no cookies (2026.08.19 defaults)
s=$(ms_now); URL=$($YTDLP --no-cookies --extractor-args "youtube:player_client=visionos,web" -f bestaudio --get-url "$VIDEO" 2>/dev/null | head -n 1); e=$(elapsed "$s" "$(ms_now)")
if [ -n "$URL" ]; then read -r CODE BYTES <<<"$(fetch_bytes "$URL")"; else CODE="---"; BYTES=0; fi
http_ok "$CODE" && V="WORKS" || V="FAIL($CODE)"
record "1c ytdlp visionos,web" "$e" "$([ -n "$URL" ] && echo yes || echo no)" "$CODE" "$BYTES" "$V"

# 1d. yt-dlp android_vr, no cookies (guide says no PO token — #17456 says its URLs 403)
s=$(ms_now); URL=$($YTDLP --no-cookies --extractor-args "youtube:player_client=android_vr" -f bestaudio --get-url "$VIDEO" 2>/dev/null | head -n 1); e=$(elapsed "$s" "$(ms_now)")
if [ -n "$URL" ]; then read -r CODE BYTES <<<"$(fetch_bytes "$URL")"; else CODE="---"; BYTES=0; fi
http_ok "$CODE" && V="WORKS" || V="FAIL($CODE)"
record "1d ytdlp android_vr" "$e" "$([ -n "$URL" ] && echo yes || echo no)" "$CODE" "$BYTES" "$V"

# 1e. InnerTube ANDROID_VR player via curl (mirrors Parlay Go fetchVideo: itag 251 then 140)
VID="$($PY -c "import re,sys; m=re.search(r'(?:v=|/)([A-Za-z0-9_-]{11})', '$VIDEO'); print(m.group(1) if m else '')" 2>/dev/null)"
s=$(ms_now)
RESP=$(curl -s --max-time 30 -H "Content-Type: application/json" -H "User-Agent: Mozilla/5.0 (Linux; Android 13) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Mobile Safari/537.36" -d "{\"context\":{\"client\":{\"clientName\":\"ANDROID_VR\",\"clientVersion\":\"1.62.27\"}},\"videoId\":\"$VID\"}" "https://www.youtube.com/youtubei/v1/player?prettyPrint=false" 2>/dev/null)
e=$(elapsed "$s" "$(ms_now)")
URL="$($PY -c "
import sys,json
try:
    d=json.load(sys.stdin)
    fmts=d.get('streamingData',{}).get('adaptiveFormats',[])
    for it in (251,140):
        for f in fmts:
            if f.get('itag')==it and f.get('url'):
                print(f['url']); sys.exit(0)
    print('CIPHERED' if any(f.get('signatureCipher') for f in fmts) else 'NONE')
except Exception:
    print('ERROR')
" <<<"$RESP")"
if [ "$URL" = "CIPHERED" ] || [ "$URL" = "NONE" ] || [ "$URL" = "ERROR" ] || [ -z "$URL" ]; then
    record "1e innertube ANDROID_VR" "$e" "no" "---" "0" "FAIL($URL)"
else
    read -r CODE BYTES <<<"$(fetch_bytes "$URL")"
    http_ok "$CODE" && V="WORKS" || V="FAIL($CODE)"
    record "1e innertube ANDROID_VR" "$e" "yes" "$CODE" "$BYTES" "$V"
fi

# 1f. yt-dlp player_client=all, no cookies (whole ladder in one pass)
s=$(ms_now); URL=$($YTDLP --no-cookies --extractor-args "youtube:player_client=all" -f bestaudio --get-url "$VIDEO" 2>/dev/null | head -n 1); e=$(elapsed "$s" "$(ms_now)")
if [ -n "$URL" ]; then read -r CODE BYTES <<<"$(fetch_bytes "$URL")"; else CODE="---"; BYTES=0; fi
http_ok "$CODE" && V="WORKS" || V="FAIL($CODE)"
record "1f ytdlp all clients" "$e" "$([ -n "$URL" ] && echo yes || echo no)" "$CODE" "$BYTES" "$V"
echo ""

# ---------- Approach 2: with cookies file ----------
echo "--- Approach 2: with cookies file ---"
if [ -z "$COOKIE_FILE" ]; then
    echo "SKIPPED: no cookies file found (pass one as \$2 or set YTDLP_COOKIEFILE). Run on the VPS."
else
    echo "cookies: $COOKIE_FILE"
    # 2a. yt-dlp defaults + cookies (shows the logged-in client-set switch)
    s=$(ms_now); URL=$($YTDLP --cookies "$COOKIE_FILE" -f bestaudio --get-url "$VIDEO" 2>/dev/null | head -n 1); e=$(elapsed "$s" "$(ms_now)")
    if [ -n "$URL" ]; then read -r CODE BYTES <<<"$(fetch_bytes "$URL")"; else CODE="---"; BYTES=0; fi
    http_ok "$CODE" && V="WORKS" || V="FAIL($CODE)"
    record "2a ytdlp defaults+cookies" "$e" "$([ -n "$URL" ] && echo yes || echo no)" "$CODE" "$BYTES" "$V"
    # 2b. yt-dlp tv + cookies
    s=$(ms_now); URL=$($YTDLP --cookies "$COOKIE_FILE" --extractor-args "youtube:player_client=tv" -f bestaudio --get-url "$VIDEO" 2>/dev/null | head -n 1); e=$(elapsed "$s" "$(ms_now)")
    if [ -n "$URL" ]; then read -r CODE BYTES <<<"$(fetch_bytes "$URL")"; else CODE="---"; BYTES=0; fi
    http_ok "$CODE" && V="WORKS" || V="FAIL($CODE)"
    record "2b ytdlp tv+cookies" "$e" "$([ -n "$URL" ] && echo yes || echo no)" "$CODE" "$BYTES" "$V"
    # 2c. yt-dlp defaults + cookies + drop web_creator (the note's fix)
    s=$(ms_now); URL=$($YTDLP --cookies "$COOKIE_FILE" --extractor-args "youtube:player_client=default,-web_creator" -f bestaudio --get-url "$VIDEO" 2>/dev/null | head -n 1); e=$(elapsed "$s" "$(ms_now)")
    if [ -n "$URL" ]; then read -r CODE BYTES <<<"$(fetch_bytes "$URL")"; else CODE="---"; BYTES=0; fi
    http_ok "$CODE" && V="WORKS" || V="FAIL($CODE)"
    record "2c cookies -web_creator" "$e" "$([ -n "$URL" ] && echo yes || echo no)" "$CODE" "$BYTES" "$V"
fi
echo ""

# ---------- Approach 3: API-only (NubCoders) ----------
echo "--- Approach 3: API-based only (NubCoders /info) ---"
if [ -z "$TOKEN" ]; then
    echo "SKIPPED: no NUBCODERS_TOKEN and no built-in default found in resolver/nubcoders.go"
else
    Q="$($PY -c "import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))" "$VIDEO" 2>/dev/null)"
    s=$(ms_now)
    INFO=$(curl -s --max-time 30 "https://api.nubcoders.com/info?token=$TOKEN&q=$Q" 2>/dev/null)
    e=$(elapsed "$s" "$(ms_now)")
    SURL="$(jget "$INFO" "d.get('stream_url','')")"
    TITLE="$(jget "$INFO" "d.get('title','')")"
    if [ -n "$SURL" ]; then
        read -r CODE BYTES <<<"$(fetch_bytes "$SURL")"
        http_ok "$CODE" && V="WORKS" || V="FAIL($CODE)"
        record "3a nubcoders /info" "$e" "yes" "$CODE" "$BYTES" "$V ($TITLE)"
    else
        record "3a nubcoders /info" "$e" "no" "---" "0" "FAIL(no stream_url: $(echo "$INFO" | head -c 120))"
    fi
fi
echo ""
echo "=== summary (time = URL extraction only; bytes = first $RANGE B of the stream) ==="
printf "%b\n" "$RESULTS" | grep -v "^$"
