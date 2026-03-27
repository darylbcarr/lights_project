#!/usr/bin/env bash
# Convenience wrapper — sets up ESP-IDF environment then forwards all args to idf.py
#
# Usage:
#   ./build.sh build                          Build firmware
#   ./build.sh -p /dev/ttyUSB0 flash         Flash (triggers full CMake/ninja check)
#   ./build.sh -p /dev/ttyUSB0 flash monitor  Flash + open monitor (same, slow)
#   ./build.sh -p /dev/ttyUSB0 flash-fast     Flash pre-built artifacts via esptool only
#   ./build.sh -p /dev/ttyUSB0 flash-fast monitor  Flash fast + open monitor
#   ./build.sh menuconfig

set -e

IDF_PATH=/home/daryl/esp/esp-idf
TOOLCHAIN=/home/daryl/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin
PYTHON=/home/daryl/.espressif/python_env/idf5.4_py3.12_env/bin/python3
ROM_ELFS=/home/daryl/.espressif/tools/esp-rom-elfs/20241011
ESPTOOL="$PYTHON -m esptool"
BUILD_DIR="$(dirname "$0")/build"

export IDF_PATH
export ESP_ROM_ELF_DIR=$ROM_ELFS
export PATH="$TOOLCHAIN:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

# Use ccache if available — dramatically speeds up rebuilds of the Matter SDK
# when only clock source files changed.
if command -v ccache >/dev/null 2>&1; then
    export IDF_CCACHE_ENABLE=1
fi

# Prefer Ninja over Unix Makefiles when available.
# Ninja uses content hashing, not timestamps, so cmake reconfigures
# (triggered by git commits updating .git/refs/heads/main) don't
# cause spurious full rebuilds of unchanged source files.
if command -v ninja >/dev/null 2>&1; then
    export IDF_CMAKE_GENERATOR=Ninja
fi

# ── Parse -p PORT from args ───────────────────────────────────────────────────
PORT=""
REST_ARGS=()
WANT_MONITOR=false
WANT_FLASH_FAST=false

i=1
while [[ $i -le $# ]]; do
    arg="${!i}"
    if [[ "$arg" == "-p" ]]; then
        i=$((i+1)); PORT="${!i}"
    elif [[ "$arg" == "flash-fast" ]]; then
        WANT_FLASH_FAST=true
    elif [[ "$arg" == "monitor" ]]; then
        WANT_MONITOR=true
    else
        REST_ARGS+=("$arg")
    fi
    i=$((i+1))
done

# ── flash-fast: bypass CMake/ninja, call esptool directly ────────────────────
if $WANT_FLASH_FAST; then
    if [[ -z "$PORT" ]]; then
        echo "Usage: ./build.sh -p /dev/ttyUSB0 flash-fast [monitor]" >&2
        exit 1
    fi
    if [[ ! -f "$BUILD_DIR/clock_project.bin" ]]; then
        echo "No build found — run ./build.sh build first." >&2
        exit 1
    fi
    echo "==> Flashing pre-built firmware to $PORT (skipping CMake/ninja)..."
    $ESPTOOL --chip esp32s3 -p "$PORT" -b 460800 \
        --before default_reset --after hard_reset write_flash \
        --flash_mode dio --flash_size 16MB --flash_freq 80m \
        0x0      "$BUILD_DIR/bootloader/bootloader.bin" \
        0x8000   "$BUILD_DIR/partition_table/partition-table.bin" \
        0x15000  "$BUILD_DIR/ota_data_initial.bin" \
        0x30000  "$BUILD_DIR/clock_project.bin"
    echo "==> Flash complete."
    if $WANT_MONITOR; then
        echo "==> Opening monitor (Ctrl-] to exit)..."
        exec "$PYTHON" "$IDF_PATH/tools/idf.py" -p "$PORT" monitor
    fi
    exit 0
fi

# ── Default: forward everything to idf.py ────────────────────────────────────
[[ -n "$PORT" ]] && REST_ARGS=("-p" "$PORT" "${REST_ARGS[@]}")
$WANT_MONITOR && REST_ARGS+=("monitor")
exec "$PYTHON" "$IDF_PATH/tools/idf.py" "${REST_ARGS[@]}"
