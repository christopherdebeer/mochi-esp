#!/usr/bin/env bash
#
# Refresh the bundled boot splash from mochi.val.run.
#
# firmware/main/assets/splash.bin is COMMITTED to the repo so that
# CI builds (and clean clones) are hermetic — no network call during
# `idf.py build`. Run this script whenever the splash-v1 artwork
# changes server-side and commit the resulting diff alongside any
# other changes in the PR.
#
# Falls back to /devsprite/test (96×96 fox + caption, ~5 KB) if
# /devsprite/splash-v1/boot 404s — e.g. before any artwork has been
# uploaded to mochi's sheets admin.
#
# No Pet-Id header sent — this is the brand-themed bundled default.
# Per-pet splash variants (planned, server-side dispatch) will use
# the runtime fetch path, not this build-time bundle.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="$SCRIPT_DIR/../main/assets"
OUT="$OUT_DIR/splash.bin"
mkdir -p "$OUT_DIR"

PRIMARY_URL="https://mochi.val.run/devsprite/splash-v1/boot"
FALLBACK_URL="https://mochi.val.run/devsprite/test"
EXPECTED_BYTES=5000

# Portable file-size in bytes — Linux uses `stat -c`, macOS uses `stat -f`.
file_size() {
  if stat -c%s "$1" >/dev/null 2>&1; then
    stat -c%s "$1"
  else
    stat -f%z "$1"
  fi
}

fetch_url() {
  local url="$1"
  if ! curl -fsSL -o "$OUT.tmp" "$url"; then
    rm -f "$OUT.tmp"
    return 1
  fi
  local size
  size=$(file_size "$OUT.tmp")
  if [ "$size" != "$EXPECTED_BYTES" ]; then
    echo "  expected $EXPECTED_BYTES bytes, got $size from $url" >&2
    rm -f "$OUT.tmp"
    return 1
  fi
  mv "$OUT.tmp" "$OUT"
  return 0
}

echo "fetching splash from $PRIMARY_URL"
if fetch_url "$PRIMARY_URL"; then
  echo "splash.bin updated ($(file_size "$OUT") bytes) — splash-v1/boot"
  exit 0
fi

echo "primary not available — falling back to $FALLBACK_URL"
if fetch_url "$FALLBACK_URL"; then
  echo "splash.bin placeholder updated ($(file_size "$OUT") bytes) — from /devsprite/test"
  echo
  echo "  Note: this is a fallback. Upload artwork to splash-v1 on mochi"
  echo "  and re-run this script to pick up the real splash."
  exit 0
fi

echo "both endpoints failed — splash.bin not updated" >&2
exit 1
