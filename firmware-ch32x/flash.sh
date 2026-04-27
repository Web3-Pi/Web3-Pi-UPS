#!/bin/bash
# Flash CH32X035 USB-PD firmware via wchisp on macOS.
#
# Modes:
#   ./flash.sh                Dev re-flash. Assumes chip is already unprotected.
#   ./flash.sh --first        First flash on a fresh CH32X035. Runs `wchisp config unprotect`
#                             before flashing (factory chips ship with RDPR=0xFF).
#   ./flash.sh --prod         Production flash. Skips verify and leaves RDPR engaged so
#                             end users cannot read firmware out of the chip.
#
# Build the .hex in MounRiver Studio 2 first; this script does not build.

set -euo pipefail

HEX="obj/USB-PD.hex"
MODE="${1:-dev}"

case "$MODE" in
  -h|--help)
    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
  dev|--first|--prod) ;;
  *)
    echo "Unknown mode: $MODE (use --first, --prod, or no arg for dev)" >&2
    exit 2
    ;;
esac

cd "$(dirname "$0")"

if [ ! -f "$HEX" ]; then
  echo "Missing $HEX — build the project in MRS2 first." >&2
  exit 1
fi

wait_for_boot() {
  echo "Waiting for CH32X035 in USB ISP boot mode..."
  until wchisp probe 2>/dev/null | grep -q CH32X035; do sleep 0.3; done
}

wait_for_boot

if [ "$MODE" = "--first" ]; then
  echo "Unprotecting flash (one-time per chip)..."
  wchisp config unprotect
  wait_for_boot   # unprotect resets the chip — re-enter boot mode
fi

if [ "$MODE" = "--prod" ]; then
  wchisp flash --no-verify "$HEX"
else
  wchisp flash "$HEX"
fi

echo "✓ Flashed at $(date '+%H:%M:%S')"
