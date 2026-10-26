#!/usr/bin/env bash
# Compile the KevFM radio firmware with the Arduino IDE's bundled arduino-cli.
# Output: build/ (yoRadio.ino.bin, bootloader, partitions, merged image)
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
CLI="${ARDUINO_CLI:-/c/Program Files/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe}"
CFG="${ARDUINO_CLI_CONFIG:-$HOME/.arduinoIDE/arduino-cli.yaml}"
FQBN="esp32:esp32:esp32:PartitionScheme=min_spiffs,PSRAM=disabled,CPUFreq=240,FlashMode=qio,FlashFreq=80,FlashSize=4M,UploadSpeed=921600,DebugLevel=none"
python "$HERE/tools/embed_web.py"
"$CLI" --config-file "$CFG" compile --fqbn "$FQBN" --output-dir "$HERE/build" --warnings default "$HERE/yoRadio" "$@"
