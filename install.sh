#!/usr/bin/env bash
# Parlay — one-command setup for a fresh Ubuntu 22.04+ / Debian 12 x86_64 VPS.
#
# Installs the 2-process engine (FINAL-ARCHITECTURE.md): the C++ core
# (services/core-cpp) + the Go unified backend (services/backend-go),
# the persistent yt-dlp daemon venv (scripts/yt-dlp-venv), and the
# optional slskd daemon. No Docker, no Mongo/Redis, no Node services.
#
# Usage:
#   ./install.sh [--from-repo] [--dir <path>]
#
# Default mode downloads the v0.3.0 release tarball and extracts it here.
# --from-repo clones https://github.com/filewrap/ParlayMusic.git instead
# (use when no release asset exists yet, or you want the live tree).
# If this directory is already a Parlay checkout (tarball root or repo
# root), the download/clone step is skipped — the script is idempotent
# and safe to re-run: it never overwrites an existing .env and never
# re-downloads anything already present.
#
# Non-interactive use: export BOT_TOKEN, API_ID, API_HASH, OWNER_ID
# (and optionally PARLAY_TOKEN) before running; the script reads them
# from the environment and never prompts. Interactively it prompts
# (token/hash read without echo) and never prints any secret value.
#
# Overrides (tests / mirrors only):
#   PARLAY_TARBALL_URL, PARLAY_REPO_URL, SLSKD_LOCAL_ZIP
set -euo pipefail

PARLAY_VERSION="v0.3.0"
TARBALL_URL="${PARLAY_TARBALL_URL:-https://github.com/filewrap/ParlayMusic/releases/download/v0.3.0/parlay-v0.3.0-linux-x86_64.tar.gz}"
REPO_URL="${PARLAY_REPO_URL:-https://github.com/filewrap/ParlayMusic.git}"
SLSKD_VERSION="0.26.2"
SLSKD_URL="https://github.com/slskd/slskd/releases/download/${SLSKD_VERSION}/slskd-${SLSKD_VERSION}-linux-x64.zip"

FROM_REPO=0
INSTALL_DIR=""

while [ $# -gt 0 ]; do
    case "$1" in
        --from-repo) FROM_REPO=1; shift ;;
        --dir) INSTALL_DIR="$2"; shift 2 ;;
        --dir=*) INSTALL_DIR="${1#--dir=}"; shift ;;
        -h|--help)
            echo "Usage: $0 [--from-repo] [--dir <path>]"
            echo "  (no args)   download + extract the ${PARLAY_VERSION} release tarball here"
            echo "  --from-repo clone ${REPO_URL} instead of the tarball"
            echo "  --dir DIR   install into DIR instead of this script's directory"
            exit 0 ;;
        *) echo "error: unknown argument: $1 (see --help)" >&2; exit 2 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -n "$INSTALL_DIR" ]; then
    ROOT="$(cd "$INSTALL_DIR" 2>/dev/null && pwd || echo "$INSTALL_DIR")"
    mkdir -p "$ROOT"
    ROOT="$(cd "$ROOT" && pwd)"
elif [ -f "$SCRIPT_DIR/docker-compose.yml" ] || [ -d "$SCRIPT_DIR/services" ]; then
    ROOT="$SCRIPT_DIR"
else
    ROOT="$PWD"
fi
cd "$ROOT"

say()  { printf '%s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- OS check
[ -f /etc/os-release ] || die "cannot detect OS (no /etc/os-release); this installer supports Ubuntu 22.04+ / Debian 12 x86_64 only."
# shellcheck disable=SC1091
. /etc/os-release
ARCH="$(uname -m)"
[ "$ARCH" = "x86_64" ] || die "unsupported architecture: $ARCH (x86_64 only)."
case "${ID:-}" in
    ubuntu)
        major="${VERSION_ID%%.*}"
        [ "$major" -ge 22 ] 2>/dev/null || die "unsupported Ubuntu ${VERSION_ID}; need Ubuntu 22.04 or newer."
        ;;
    debian)
        major="${VERSION_ID%%.*}"
        [ "$major" -ge 12 ] 2>/dev/null || die "unsupported Debian ${VERSION_ID}; need Debian 12 or newer."
        ;;
    *) die "unsupported OS: ${NAME:-$ID} (need Ubuntu 22.04+ or Debian 12, x86_64)." ;;
esac
say "OS: ${NAME:-$ID} ${VERSION_ID:-} ($ARCH) — supported."

# ---------------------------------------------------------------- apt deps
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    command -v sudo >/dev/null 2>&1 || die "not root and no sudo available; re-run as root or install sudo."
    SUDO="sudo"
fi

MISSING_PKGS=()
command -v curl    >/dev/null 2>&1 || MISSING_PKGS+=(curl)
command -v git     >/dev/null 2>&1 || MISSING_PKGS+=(git)
command -v python3 >/dev/null 2>&1 || MISSING_PKGS+=(python3)
python3 -c "import venv" 2>/dev/null || MISSING_PKGS+=(python3-venv)
command -v pip3    >/dev/null 2>&1 || MISSING_PKGS+=(python3-pip)
command -v ffmpeg  >/dev/null 2>&1 || MISSING_PKGS+=(ffmpeg)
# Phase 5: libparlay_dsp.so links libopus.so.0 directly (hand-rolled
# FFI — symphonia 0.5 has no Opus codec). ffmpeg normally pulls
# libopus0 in as a dependency, but check explicitly: a static ffmpeg
# build would leave the DSP unloadable at runtime.
ldconfig -p 2>/dev/null | grep -q "libopus\.so\.0" || MISSING_PKGS+=(libopus0)
command -v unzip   >/dev/null 2>&1 || MISSING_PKGS+=(unzip)
command -v openssl >/dev/null 2>&1 || MISSING_PKGS+=(openssl)

if [ "${#MISSING_PKGS[@]}" -gt 0 ]; then
    say "Installing missing packages via apt: ${MISSING_PKGS[*]}"
    export DEBIAN_FRONTEND=noninteractive
    $SUDO apt-get update -qq || warn "apt-get update failed; continuing — packages may already be present."
    $SUDO apt-get install -y -qq "${MISSING_PKGS[@]}" \
        || warn "apt-get install reported a failure; continuing — will verify each tool below."
fi

# Verify every tool we need is now present; fail clearly if not.
for tool in curl git python3 ffmpeg unzip openssl; do
    command -v "$tool" >/dev/null 2>&1 || die "required tool '$tool' is still missing after apt install."
done
python3 -c "import venv" 2>/dev/null || die "python3-venv is still missing (python3 -m venv unavailable)."

python3 -c "import venv" 2>/dev/null || die "python3-venv is still missing (python3 -m venv unavailable)."
say "Tools OK: python3 $(python3 --version | awk '{print $2}'), ffmpeg $(ffmpeg -version 2>/dev/null | head -1 | awk '{print $3}')."

# ---------------------------------------------------------------- source tree
is_checkout() { # repo root (services/backend-go + services/core-cpp)
                # OR tarball root (bin/parlay-core + bin/parlay-backend)
    [ -d "$ROOT/services/backend-go" ] && [ -d "$ROOT/services/core-cpp" ] \
        || { [ -x "$ROOT/bin/parlay-core" ] && [ -x "$ROOT/bin/parlay-backend" ]; }
}

if is_checkout; then
    say "Parlay checkout already present at $ROOT — skipping download/clone."
elif [ "$FROM_REPO" -eq 1 ]; then
    if [ -n "$(ls -A "$ROOT" 2>/dev/null)" ]; then
        die "--from-repo needs an empty directory; $ROOT is not empty and is not a Parlay checkout."
    fi
    say "Cloning ${REPO_URL} …"
    git clone --depth 1 "$REPO_URL" "$ROOT" \
        || die "git clone failed. Check network access to github.com, then re-run $0."
    say "Cloned. NOTE: a repo checkout has no prebuilt binaries — build the"
    say "C++ core and Go backend first (see README.md):"
    say "  cmake -S services/core-cpp -B services/core-cpp/build -DCMAKE_BUILD_TYPE=Release"
    say "  cmake --build services/core-cpp/build"
    say "  (cd services/backend-go && go build -trimpath -o ../../bin/parlay-backend ./cmd/backend)"
    say "or use the release tarball instead (./start.sh builds the backend"
    say "on demand when a Go toolchain is present)."
else
    if [ -x "$ROOT/bin/parlay-core" ]; then
        say "Release already extracted (bin/parlay-core present) — skipping download."
    else
        say "Downloading Parlay ${PARLAY_VERSION} release tarball…"
        TMP_TGZ="$(mktemp /tmp/parlay-release-XXXXXX.tar.gz)"
        trap 'rm -f "$TMP_TGZ"' EXIT
        curl -fsSL -o "$TMP_TGZ" "$TARBALL_URL" \
            || die "download failed: $TARBALL_URL — check the URL / network, or re-run with --from-repo to clone the source instead."
        tar -xzf "$TMP_TGZ" -C "$ROOT" --strip-components=1 \
            || die "failed to extract the release tarball."
        rm -f "$TMP_TGZ"
        trap - EXIT
        if [ -f "$ROOT/SHA256SUMS" ]; then
            ( cd "$ROOT" && sha256sum -c SHA256SUMS >/dev/null 2>&1 ) \
                && say "Tarball checksums verified (SHA256SUMS)." \
                || warn "SHA256SUMS present but verification failed — files may be incomplete; continuing."
        fi
    fi
fi
if ! is_checkout; then
    # An old (pre-v0.3.0) release tarball predates the unified engine
    # (no services/backend-go, no bin/parlay-backend): it cannot be
    # installed by this script. Use --from-repo, or a v0.3.0+ release
    # cut from the unified tree.
    if [ -x "$ROOT/bin/parlay-core" ]; then
        die "the tree at $ROOT predates the unified engine (bin/parlay-core present but no backend-go / bin/parlay-backend) — re-run with --from-repo, or wait for a release cut from the unified tree."
    fi
    die "no Parlay checkout at $ROOT after download/clone (expected services/backend-go + services/core-cpp, or bin/parlay-core + bin/parlay-backend)."
fi

# ---------------------------------------------------------------- dirs + venv
# One shared data dir for the whole engine: the bbolt store
# (data/parlay.db), the backend UDS socket (data/parlay-backend.sock),
# downloads (data/downloads), the TDLib session dirs (data/tdlib-bot
# for the bot, data/tdlib for the assistant — the core creates them
# mode 0700 itself), and the optional slskd app dir (data/slskd).
mkdir -p "$ROOT/data" "$ROOT/data/downloads" "$ROOT/data/slskd" "$ROOT/logs" "$ROOT/run" "$ROOT/bin" "$ROOT/lib"

# The persistent yt-dlp daemon runs on this venv's python
# (scripts/yt-dlp-venv/bin/python); the Go backend spawns it at boot
# and locates it by walking up from its working directory. System
# python3 does NOT have yt_dlp — the venv is the only yt-dlp path.
YTDLP_VENV="$ROOT/scripts/yt-dlp-venv"
if [ ! -x "$YTDLP_VENV/bin/python" ]; then
    say "Creating yt-dlp venv at $YTDLP_VENV …"
    python3 -m venv "$YTDLP_VENV" || die "python3 -m venv failed."
fi
if "$YTDLP_VENV/bin/python" -c "import yt_dlp" 2>/dev/null; then
    say "yt-dlp venv already has yt_dlp — skipping pip install."
else
    say "Installing yt-dlp into the daemon venv…"
    if ! "$YTDLP_VENV/bin/pip" install -q yt-dlp; then
        warn "pip install failed (network?). The backend still starts,"
        warn "but the yt-dlp fallback (age-gated/ciphered YouTube,"
        warn "TikTok metadata, YouTube/Spotify downloads) stays down"
        warn "until you run:"
        warn "  $YTDLP_VENV/bin/pip install yt-dlp"
    fi
fi

# ---------------------------------------------------------------- slskd (opt-in)
if [ -x "$ROOT/bin/slskd" ] && "$ROOT/bin/slskd" --version >/dev/null 2>&1; then
    say "slskd already present: $("$ROOT/bin/slskd" --version | head -1)"
else
    say "Fetching slskd ${SLSKD_VERSION} (opt-in Soulseek daemon)…"
    TMP_ZIP="$(mktemp /tmp/slskd-XXXXXX.zip)"
    trap 'rm -f "$TMP_ZIP"' EXIT
    SLSKD_OK=0
    if [ -n "${SLSKD_LOCAL_ZIP:-}" ] && [ -f "$SLSKD_LOCAL_ZIP" ]; then
        cp "$SLSKD_LOCAL_ZIP" "$TMP_ZIP" && SLSKD_OK=1
    elif curl -fsSL -o "$TMP_ZIP" "$SLSKD_URL"; then
        SLSKD_OK=1
    fi
    if [ "$SLSKD_OK" -eq 1 ] \
        && unzip -o -q "$TMP_ZIP" -d "$ROOT/bin/slskd-bin" \
        && [ -x "$ROOT/bin/slskd-bin/slskd" ]; then
        ln -sf slskd-bin/slskd "$ROOT/bin/slskd"
        if "$ROOT/bin/slskd" --version >/dev/null 2>&1; then
            say "slskd OK: $("$ROOT/bin/slskd" --version | head -1)"
        else
            warn "slskd extracted but --version failed; continuing without it."
        fi
    else
        warn "slskd download failed — Soulseek is opt-in, continuing without it."
        say "  To add slskd later, run exactly:"
        say "    curl -L -o /tmp/slskd.zip $SLSKD_URL"
        say "    unzip -o /tmp/slskd.zip -d $ROOT/bin/slskd-bin"
        say "    ln -sf slskd-bin/slskd $ROOT/bin/slskd"
        say "    $ROOT/bin/slskd --version"
    fi
    rm -f "$TMP_ZIP"
    trap - EXIT
fi

# ---------------------------------------------------------------- .env
# Never overwrite an existing .env. Values are prompted (token/hash
# without echo) or taken from the environment for non-interactive use.
# Nothing secret is ever printed.
upsert_env() { # file key value — replace the KEY= line, or append it
    local file="$1" key="$2" value="$3"
    grep -v "^${key}=" "$file" > "$file.tmp" && mv "$file.tmp" "$file"
    printf '%s=%s\n' "$key" "$value" >> "$file"
}

if [ -f "$ROOT/.env" ]; then
    say ".env already exists — keeping it (never overwritten by install.sh)."
else
    if [ -f "$ROOT/.env.example" ]; then
        cp "$ROOT/.env.example" "$ROOT/.env"
    else
        printf '# Parlay — generated by install.sh. Never commit this file.\nBOT_TOKEN=\nAPI_ID=\nAPI_HASH=\nOWNER_ID=\nPARLAY_TOKEN=\n# Spotify OAuth redirect URI — must match the Spotify app exactly.\nSPOTIFY_REDIRECT_URI=http://127.0.0.1:8080/callback\n' > "$ROOT/.env"
    fi
    chmod 600 "$ROOT/.env"

    prompt_value() { # varname prompt secret(0/1)
        local var="$1" prompt="$2" secret="$3" val=""
        val="${!var:-}"
        if [ -n "$val" ]; then printf '%s' "$val"; return 0; fi
        if [ ! -t 0 ]; then return 1; fi
        if [ "$secret" = "1" ]; then
            printf '%s: ' "$prompt" >&2
            IFS= read -rs val || true
            printf '\n' >&2
        else
            printf '%s: ' "$prompt" >&2
            IFS= read -r val || true
        fi
        printf '%s' "$val"
    }

    BOT_TOKEN="$(prompt_value BOT_TOKEN "BOT_TOKEN (from @BotFather)" 1)" \
        || die "BOT_TOKEN is required: export BOT_TOKEN=… and re-run, or run interactively."
    API_ID="$(prompt_value API_ID "API_ID (Telegram api id)" 0)" \
        || die "API_ID is required: export API_ID=… and re-run, or run interactively."
    API_HASH="$(prompt_value API_HASH "API_HASH (Telegram api hash)" 1)" \
        || die "API_HASH is required: export API_HASH=… and re-run, or run interactively."
    OWNER_ID="$(prompt_value OWNER_ID "OWNER_ID (your Telegram user id; empty = public commands only)" 0)" || OWNER_ID=""

    PARLAY_TOKEN="${PARLAY_TOKEN:-}"
    if [ -z "$PARLAY_TOKEN" ]; then
        PARLAY_TOKEN="$(openssl rand -hex 32)"
        say "Generated a random PARLAY_TOKEN (shared IPC token for all services)."
    fi

    upsert_env "$ROOT/.env" BOT_TOKEN "$BOT_TOKEN"
    upsert_env "$ROOT/.env" API_ID "$API_ID"
    upsert_env "$ROOT/.env" API_HASH "$API_HASH"
    upsert_env "$ROOT/.env" OWNER_ID "$OWNER_ID"
    upsert_env "$ROOT/.env" PARLAY_TOKEN "$PARLAY_TOKEN"
    chmod 600 "$ROOT/.env"

    # Soulseek is opt-in: disabled by default; enable by setting
    # SLSKD_ENABLED=1 plus a Soulseek username/password in .env.
    if ! grep -q '^SLSKD_ENABLED=' "$ROOT/.env"; then
        printf '\n# Soulseek (opt-in): set SLSKD_ENABLED=1 and a Soulseek account to start slskd.\nSLSKD_ENABLED=0\nSLSKD_SLSK_USERNAME=\nSLSKD_SLSK_PASSWORD=\n' >> "$ROOT/.env"
    fi
    say ".env written (mode 600)."
fi

# ---------------------------------------------------------------- validate
# shellcheck disable=SC1091
set -a; . "$ROOT/.env"; set +a
FAIL=""
case "${BOT_TOKEN:-}" in
    [0-9]*:*[A-Za-z0-9_-]*) : ;;
    *) FAIL="${FAIL} BOT_TOKEN (must look like digits:…)" ;;
esac
case "${API_ID:-}" in
    ''|*[!0-9]*) FAIL="${FAIL} API_ID (must be numeric)" ;;
esac
[ -n "${API_HASH:-}" ] || FAIL="${FAIL} API_HASH (must be non-empty)"
case "${OWNER_ID:-}" in
    ''|[0-9]*) : ;;
    *) FAIL="${FAIL} OWNER_ID (must be numeric when set)" ;;
esac
if [ -n "$FAIL" ]; then
    die ".env validation failed on:${FAIL} — fix $ROOT/.env and re-run ./install.sh (it will not overwrite your values)."
fi
[ -n "${OWNER_ID:-}" ] || warn "OWNER_ID is empty — the bot will serve public commands only until you set it."
chmod 600 "$ROOT/.env"

# ---------------------------------------------------------------- done
chmod +x "$ROOT/install.sh" "$ROOT/start.sh" "$ROOT/stop.sh" "$ROOT/status.sh" 2>/dev/null || true
say ""
say "Parlay ${PARLAY_VERSION} installed at $ROOT."
say "Next step: ./start.sh"
