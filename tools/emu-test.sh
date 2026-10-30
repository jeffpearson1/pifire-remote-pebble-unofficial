#!/bin/bash
# Usage: tools/emu-test.sh <platform> [pifire url]
# Points the emulator's saved settings at a PiFire (default: the mock on :8080),
# then rebuilds and installs the app on that emulator.
set -e
export PATH="$HOME/.local/bin:/usr/bin:/bin"
PLATFORM="${1:-basalt}"
URL="${2:-http://localhost:8080}"
UUID=5f56a0d6-4224-48f0-bfc0-b4f85106fd2b
SDK_VER=$(basename "$(readlink -f ~/.local/share/pebble-sdk/SDKs/current)")
STORE=~/.local/share/pebble-sdk/$SDK_VER/$PLATFORM/localstorage/$UUID

pebble kill >/dev/null 2>&1 || true
mkdir -p "$(dirname "$STORE")"
python3 -I -c '
import dbm.dumb, json, sys
db = dbm.dumb.open(sys.argv[1], "c")
db["clay-settings"] = json.dumps({"PIFIRE_URL": sys.argv[2]})
db.close()
' "$STORE" "$URL"

bash "$(dirname "$0")/../build-wsl.sh" "$(wslpath -w "$(dirname "$0")/..")" | tail -1
cd ~/pifire-remote
pebble install --emulator "$PLATFORM" 2>&1 | tail -1
