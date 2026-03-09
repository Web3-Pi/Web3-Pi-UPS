#!/bin/bash
set -e

SRC="/Users/cmd0s/data/w3p/W3P_UPS/USB-PD/obj/USB-PD.hex"
DST="/Volumes/SHARED_DATA/FW/USB-PD.hex"
TMP="/Volumes/SHARED_DATA/FW/.USB-PD.hex.tmp"

echo "Watching: $SRC"
echo "Copy to:   $DST"

fswatch -o "$SRC" | while read -r _; do
  if [ -f "$SRC" ] && [ -d "$(dirname "$DST")" ]; then
    cp "$SRC" "$TMP"
    mv -f "$TMP" "$DST"
    echo "Copied at $(date '+%Y-%m-%d %H:%M:%S')"
  else
    echo "Skip - source missing or share not mounted"
  fi
done

