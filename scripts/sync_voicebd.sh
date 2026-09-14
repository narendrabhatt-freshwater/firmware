#!/usr/bin/env bash
# Explicitly sync the firmware-owned implementation; never copy voicebd.h.
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <mainframe-root>" >&2
  exit 2
fi
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="$ROOT/voice_bd/voicebd.cpp"
DEST="$1/mas/voicebd.cpp"
if [[ ! -f "$DEST" || -L "$DEST" ]]; then
  echo "error: expected an existing regular file: $DEST" >&2
  exit 1
fi
if cmp -s "$SOURCE" "$DEST"; then
  echo "voicebd.cpp already matches; nothing changed."
  exit 0
fi
diff_status=0
diff -u "$DEST" "$SOURCE" || diff_status=$?
if [[ $diff_status -gt 1 ]]; then
  echo "error: cannot compare source and destination" >&2
  exit "$diff_status"
fi
BACKUP="$(mktemp "${DEST}.backup.XXXXXX")"
cp -p "$DEST" "$BACKUP"
cp "$SOURCE" "$DEST"
echo "Updated $DEST"
echo "Previous version saved at $BACKUP"
