#!/bin/bash
# Flash CH32X035 USB-PD firmware via wchisp on macOS.
#
# Modes:
#   ./flash.sh                Dev re-flash. Assumes chip is already unprotected.
#   ./flash.sh --first        First flash on a fresh CH32X035. Sends WRITE_CONFIG to
#                             unprotect, then erase/flash/verify in the same USB
#                             session — only ONE BOOT+reset needed.
#                             Requires patched wchisp (with `flash --unprotect`).
#   ./flash.sh --prod         Production flash. Skips verify and leaves RDPR engaged so
#                             end users cannot read firmware out of the chip.
#
# Build the .hex in MounRiver Studio 2 first; this script does not build.

set -euo pipefail

HEX="obj/USB-PD.hex"
MODE="${1:-dev}"

case "$MODE" in
  -h|--help)
    sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
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

case "$MODE" in
  --first)
    if ! wchisp flash --help 2>/dev/null | grep -q -- '--unprotect'; then
      echo "ERROR: this wchisp build doesn't support 'flash --unprotect'." >&2
      echo "Rebuild from /Users/cmd0s/data/repos/wchisp (the local fork) and re-run." >&2
      exit 3
    fi
    wchisp flash --unprotect "$HEX"
    ;;
  --prod)
    wchisp flash --no-verify "$HEX"
    ;;
  dev)
    wchisp flash "$HEX"
    ;;
esac

echo "✓ Flashed at $(date '+%H:%M:%S')"
