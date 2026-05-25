#!/usr/bin/env bash
#
# Repeatable ESP-IDF toolchain setup for mochi-esp firmware builds.
#
# Brings a fresh checkout (laptop, CI, or a Claude Code remote/web
# session) to a buildable state. Clones ESP-IDF v5.3 (the version the
# firmware is verified against — see firmware/README.md "Toolchain")
# and installs the xtensa-esp32s3-elf toolchain into ~/.espressif.
#
# Safe to re-run: the clone is skipped when ESP-IDF is already present,
# and install.sh is itself idempotent. ~2 GB on first run.
#
# Usage:
#   firmware/scripts/setup-esp-idf.sh        # install
#   . ~/esp/esp-idf/export.sh                # activate (each shell)
#   cd firmware && idf.py set-target esp32s3 && idf.py build
#
# Override the install location or branch via env vars:
#   IDF_BASE=/opt/esp-idf IDF_BRANCH=release/v5.4 firmware/scripts/setup-esp-idf.sh

set -euo pipefail

IDF_BRANCH="${IDF_BRANCH:-release/v5.3}"
IDF_BASE="${IDF_BASE:-$HOME/esp/esp-idf}"

# System deps. install.sh pulls in the toolchain + a Python venv but
# assumes a handful of shared libraries are already present. libusb is
# the one a minimal container is most likely to be missing — without it
# the openocd tool fails to load and install.sh exits non-zero (openocd
# is JTAG-only and not needed to build, but its failure aborts the run).
if command -v apt-get >/dev/null 2>&1; then
    SUDO=""
    [ "$(id -u)" -ne 0 ] && SUDO="sudo"
    $SUDO apt-get -qq update >/dev/null 2>&1 || true
    $SUDO apt-get -qq install -y git wget flex bison gperf cmake ninja-build \
        ccache libffi-dev libssl-dev dfu-util libusb-1.0-0 python3 python3-venv \
        >/dev/null 2>&1 || true
fi

if [ -f "$IDF_BASE/export.sh" ]; then
    echo "ESP-IDF already present at $IDF_BASE (skipping clone)"
else
    echo "Cloning ESP-IDF $IDF_BRANCH into $IDF_BASE ..."
    mkdir -p "$(dirname "$IDF_BASE")"
    # Shallow clone + shallow submodules keeps the download to the
    # minimum needed to build (full history is ~hundreds of MB more).
    git clone -b "$IDF_BRANCH" --depth 1 --recursive --shallow-submodules \
        https://github.com/espressif/esp-idf.git "$IDF_BASE"
fi

echo "Installing esp32s3 toolchain (into ~/.espressif) ..."
"$IDF_BASE/install.sh" esp32s3

cat <<EOF

────────────────────────────────────────────────────────────────────
ESP-IDF ready ($IDF_BRANCH @ $IDF_BASE).

Activate it in every shell that builds:

    . "$IDF_BASE/export.sh"

Then, from the repo root:

    cd firmware
    idf.py set-target esp32s3     # one-time per checkout
    idf.py reconfigure            # one-time per clean clone (fetches managed_components/)
    idf.py build
────────────────────────────────────────────────────────────────────
EOF
