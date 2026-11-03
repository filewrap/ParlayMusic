# Parlay Mini App — TypeScript lossless player (Phase 6)
#
# Builds the typed single-page player (src/*.ts, no framework,
# no runtime dependencies) into dist/, which the Go backend
# serves (backend http_addr, [backend] miniapp_dir). TypeScript
# is a dev-only build tool (node_modules is gitignored; run
# `npm install` once after checkout).
#
# Usage:
#   services/miniapp/build.sh        # typecheck + build to dist/
#   npm run typecheck                # tsc --noEmit only
#
# State sync is a 2 s poll of /api/v1/now-playing (see
# src/app.ts): the locked spec's WebSocket/SSE instant sync is
# NOT implemented yet — polling the real state, honestly, beats
# a fake live channel. The <audio> element streams raw FLAC
# from /api/v1/stream/:chat_id/flac (HTTP Range requests).
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -x node_modules/.bin/tsc ]; then
  echo "miniapp: installing typescript (dev-only)..." >&2
  npm install --no-audit --no-fund >&2
fi

node_modules/.bin/tsc --noEmit
rm -rf dist
mkdir -p dist
node_modules/.bin/tsc
cp index.html styles.css dist/
echo "miniapp: built dist/ ($(ls dist/ | tr '\n' ' '))"
