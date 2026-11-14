#!/bin/bash
# Builds knob/deck. NimBLE keeps its memory in PSRAM (internal RAM is short with LVGL + BLE).
# Usage: knob/deck/build.sh [upload [port]]
set -euo pipefail
cd "$(dirname "$0")"
FQBN='esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB'
BUILD="${KNOB_BUILD:-/tmp/knob-build}"
FLAGS='-DCONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=1'
if ! OUT=$(arduino-cli compile --fqbn "$FQBN" --libraries ../libraries --build-path "$BUILD" \
  --build-property "compiler.cpp.extra_flags=$FLAGS" --build-property "compiler.c.extra_flags=$FLAGS" . 2>&1); then
  echo "$OUT" | grep -E 'error' | head -20
  exit 1
fi
echo "$OUT" | grep -E 'Sketch uses|Global variables' 
if [ "${1:-}" = upload ]; then
  # The relay agent holds the serial port: take it down for the upload, bring it back after.
  AGENT="gui/$(id -u)/com.floatrx.knob-relay"
  PLIST="$HOME/Library/LaunchAgents/com.floatrx.knob-relay.plist"
  LOADED=$(launchctl print "$AGENT" >/dev/null 2>&1 && echo yes || echo no)
  [ "$LOADED" = yes ] && launchctl bootout "$AGENT" && sleep 1
  PORT="${2:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
  arduino-cli upload -p "$PORT" --fqbn "$FQBN" --input-dir "$BUILD" 2>&1 | tr '\r' '\n' | grep -E 'Hard resetting|rror' | tail -1
  [ "$LOADED" = yes ] && launchctl bootstrap "gui/$(id -u)" "$PLIST"
  # Opening the port while the S3 is still booting wedged its USB-Serial-JTAG three times (2026-11-14): silent until
  # a power cycle, esptool included. Give it time before a test driver opens the port.
  sleep 8
fi
