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

# ── Boot splash PACK (design/20): a random-cell splash with text + pet
# zones, built from the spash-bundle-a sheet. Optional — when the pack
# isn't available (sheet not authored yet) this is skipped and the firmware
# falls back to splash.bin plus a default version banner. CMake embeds
# splash.mpk only when it exists (see main/CMakeLists.txt).
SPLASH_MPK_URL="https://mochi.val.run/devsprite/pack/spash-bundle-a"
MPK_OUT="$OUT_DIR/splash.mpk"
echo "fetching splash pack from $SPLASH_MPK_URL"
if curl -fsSL -o "$MPK_OUT.tmp" "$SPLASH_MPK_URL" \
   && [ "$(head -c4 "$MPK_OUT.tmp" 2>/dev/null)" = "MPK1" ]; then
  mv "$MPK_OUT.tmp" "$MPK_OUT"
  echo "splash.mpk updated ($(file_size "$MPK_OUT") bytes) — spash-bundle-a pack"
else
  rm -f "$MPK_OUT.tmp"
  echo "  splash.mpk not updated (pack unavailable) — firmware uses splash.bin" >&2
fi

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
