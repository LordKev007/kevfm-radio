#!/usr/bin/env bash
# Build build/spiffs.bin from yoRadio/data (web page files) for the KevFM partition table
# (yoRadio/partitions.csv: spiffs at 0x390000, 0x60000 = 384 KB). The station list is not in the image: the KevFM
# plugin writes it at boot.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MKSPIFFS="${MKSPIFFS:-$LOCALAPPDATA/Arduino15/packages/esp32/tools/mkspiffs/0.2.3/mkspiffs.exe}"
mkdir -p "$HERE/build"
"$MKSPIFFS" -c "$HERE/yoRadio/data" -b 4096 -p 256 -s 0x60000 "$HERE/build/spiffs.bin"
ls -l "$HERE/build/spiffs.bin"
