#!/bin/bash
# Usage: build.sh <windows project dir>
set -e
export PATH="$HOME/.local/bin:/usr/bin:/bin"
SRC="$(cd "$(wslpath -u "$1")" && pwd)"
DEST="$HOME/pifire-remote"
mkdir -p "$DEST"
rm -rf "$DEST/src"
cp -r "$SRC/package.json" "$SRC/wscript" "$SRC/src" "$DEST/"
cd "$DEST"
[ -d node_modules/@rebble/clay ] || npm install --no-audit --no-fund 2>&1 | tail -2
pebble build 2>&1 | grep -v -i "syntaxwarning\|:param\|represents a list\|s or :class" | tail -40
cp build/pifire-remote.pbw "$SRC/"
ls -la "$SRC/pifire-remote.pbw"
