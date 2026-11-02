#!/bin/sh
# build-addon.sh — compile the ntgcalls N-API addon (src/ntg_addon.cc).
# Needs libntgcalls.so built first (see phase5/ntg-build.log) and the
# Node headers at /usr/include/node/node_api.h (present on this host).
# No npm, no node-gyp, no network. Exit 0 + the .node path on success.
set -eu
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ ! -f /usr/include/node/node_api.h ]; then
  echo "build-addon: /usr/include/node/node_api.h missing — cannot build the addon" >&2
  exit 1
fi
g++ -std=c++20 -O2 -shared -fPIC -I/usr/include/node \
  "$DIR/ntg_addon.cc" -o "$DIR/ntg_addon.node" -ldl
echo "built $DIR/ntg_addon.node"
node -e "const a=require('$DIR/ntg_addon.node'); console.log('addon describe:', a.describe()); console.log('addon available:', a.available());"
