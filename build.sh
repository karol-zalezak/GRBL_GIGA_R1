#!/bin/sh
# Build (and optionally upload) Grbl-Giga for the Arduino GIGA R1 WiFi.
#
#   ./build.sh                 build both cores
#   ./build.sh COM5            build both, upload the M7 (Grbl), then the M4 (display)
#   ./build.sh COM5 m7         build and upload only the M7 part
#   ./build.sh COM5 m4         build and upload only the M4 part
#
# Both images are built with the "1.5MB M7 + 0.5MB M4" flash split; the M7
# starts the M4 from 0x08180000, and Grbl keeps its settings in two flash
# sectors just below that. Uploading goes over USB DFU (the 1200 baud touch
# puts the board into its bootloader); on Windows the bootloader needs the
# driver from the core's drivers/ folder (see README).
set -e
CLI=${ARDUINO_CLI:-arduino-cli}
FQBN=arduino:mbed_giga:giga
OPTS=split=75_25
PORT="$1"
WHICH="${2:-both}"

build() {
  # $1 = cm7 | cm4, $2 = m7 | m4
  "$CLI" compile -b "$FQBN:target_core=$1,$OPTS" --build-path "./build/bp$2" --output-dir "./build/$2" .
}

upload() {
  "$CLI" upload -b "$FQBN:target_core=$1,$OPTS" -p "$PORT" --input-dir "./build/$2" .
}

if [ "$WHICH" = "both" ] || [ "$WHICH" = "m7" ]; then build cm7 m7; fi
if [ "$WHICH" = "both" ] || [ "$WHICH" = "m4" ]; then build cm4 m4; fi

if [ -n "$PORT" ]; then
  if [ "$WHICH" = "both" ] || [ "$WHICH" = "m7" ]; then upload cm7 m7; fi
  if [ "$WHICH" = "both" ]; then sleep 3; fi
  if [ "$WHICH" = "both" ] || [ "$WHICH" = "m4" ]; then upload cm4 m4; fi
fi
