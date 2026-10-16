#!/bin/sh
# Builds (and optionally flashes) the firmware for one of the two boards.
#
#   scripts/build.sh esp32                       # ESP32-WROOM-32U DevKit
#   scripts/build.sh esp32s3                     # ESP32-S3-WROOM-2 (N32R16V) DevKit
#   scripts/build.sh esp32   flash /dev/ttyUSB0  # build + flash + monitor
#   scripts/build.sh esp32s3 flash /dev/ttyACM0
#
# Each target has its own build directory and sdkconfig, so the two builds live
# side by side and never overwrite each other:
#   build-esp32/   + sdkconfig.esp32
#   build-esp32s3/ + sdkconfig.esp32s3
# Run ". $IDF_PATH/export.sh" (ESP-IDF) before this script.
set -e
cd "$(dirname "$0")/.."

target="$1"
case "$target" in
	esp32|esp32s3) ;;
	*) echo "usage: $0 <esp32|esp32s3> [flash <port>]"; exit 1 ;;
esac

command -v idf.py >/dev/null || { echo "idf.py not found: run '. \$IDF_PATH/export.sh' first"; exit 1; }

sh scripts/gen_headers.sh

idf="idf.py -B build-$target -D SDKCONFIG=sdkconfig.$target -D IDF_TARGET=$target"
$idf build

if [ "$2" = "flash" ]; then
	port="${3:-/dev/ttyUSB0}"
	$idf -p "$port" flash monitor
fi
