#!/bin/bash
# Usage: tools/screens.sh <platform>...
# With tools/mock_pifire.py running, installs on each emulator and saves screenshots
# of every screen to screens/<platform>-<name>.png. Only talks to the mock.
export PATH="$HOME/.local/bin:/usr/bin:/bin"
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$PROJ/screens"
mkdir -p "$OUT"
MOCK=http://localhost:8080

for P in "$@"; do
  b() { pebble emu-button "$@" --emulator "$P" >/dev/null 2>&1; sleep 0.8; }
  shot() { sleep 1; pebble screenshot --no-open --emulator "$P" "$OUT/$P-$1.png" >/dev/null 2>&1; }

  curl -s "$MOCK/api/set/mode/smoke" >/dev/null
  bash "$PROJ/tools/emu-test.sh" "$P" "$MOCK" >/dev/null 2>&1
  sleep 10
  shot 1-main
  b click down;              shot 2-setpoint
  b click back
  b click select;            shot 3-menu
  b click down --repeat 4
  b click select;            shot 4-shutdown
  b click back
  b click down --repeat 3
  b click select;            shot 5-estop
  b click back; b click back
  curl -s "$MOCK/api/set/mode/stop" >/dev/null
  b click up; sleep 3;       shot 6-stopped
  echo "done $P"
done
pebble kill >/dev/null 2>&1
