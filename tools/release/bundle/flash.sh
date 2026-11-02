#!/usr/bin/env bash
# flash.sh — flash this gugu firmware bundle onto an M5Stack StopWatch from a Mac or Linux machine that
# has only Python 3. No ESP-IDF needed: the first run creates a private virtualenv with esptool and
# pyserial (needs internet once, ~30 s). The StopWatch enumerates as a USB-Serial/JTAG device without
# drivers, and esptool puts it into download mode by itself.
#
#   ./flash.sh                  bootloader + partition table + app + content image
#                               (keeps Wi-Fi, API keys and recorded calls: NVS and the data partition are untouched)
#   ./flash.sh --app-only       only the application image (seconds; prompts/presets/sounds unchanged)
#   ./flash.sh --erase          full erase first: Wi-Fi, keys and all data are gone (factory reset)
#   ./flash.sh --port /dev/cu.usbmodemXXXX   pick the port when more than one device is attached
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/flash_manifest.sh"

PORT=""; MODE="full"; ERASE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --port) PORT="$2"; shift 2 ;;
        --app-only) MODE="app"; shift ;;
        --erase) ERASE=1; shift ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

# ---- toolchain: a private virtualenv with esptool + pyserial ----
VENV="${GUGU_FLASH_VENV:-$HOME/.venvs/gugu-flash}"
PY="$VENV/bin/python"
if [ ! -x "$PY" ] || ! "$PY" -c "import esptool, serial" 2>/dev/null; then
    echo "setting up esptool in $VENV (first run only)..."
    command -v python3 >/dev/null || { echo "python3 not found: install Xcode command line tools (xcode-select --install) or python.org Python" >&2; exit 1; }
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install --quiet --upgrade pip
    "$VENV/bin/pip" install --quiet "esptool>=4.8,<5" "pyserial>=3.5"
fi

# ---- port: the Espressif USB-Serial/JTAG device (VID 303A, PID 1001) ----
if [ -z "$PORT" ]; then
    PORT="$("$PY" - <<'EOF'
from serial.tools import list_ports
hits = [p.device for p in list_ports.comports() if p.vid == 0x303A and p.pid == 0x1001]
if not hits:
    hits = [p.device for p in list_ports.comports() if 'usbmodem' in p.device or 'ttyACM' in p.device]
print(hits[0] if hits else '')
EOF
)"
fi
[ -n "$PORT" ] || { echo "no StopWatch found on USB: plug it in (data cable), or pass --port /dev/cu.usbmodemXXXX" >&2; exit 1; }

ESPTOOL=("$PY" -m esptool --chip "$CHIP" --port "$PORT" --baud 921600 --before default_reset --after hard_reset)
echo "gugu $VERSION -> $PORT ($MODE flash$( [ $ERASE = 1 ] && echo ', erase first'))"

if [ $ERASE = 1 ]; then
    "${ESPTOOL[@]}" erase_flash
fi
if [ "$MODE" = "app" ]; then LIST="$APP_IMAGE"; else LIST="$IMAGES"; fi
# shellcheck disable=SC2086
(cd "$HERE" && "${ESPTOOL[@]}" write_flash --flash_mode "$FLASH_MODE" --flash_size "$FLASH_SIZE" --flash_freq "$FLASH_FREQ" $LIST)

cat <<EOF

done. The device is rebooting. Next:
  ./console.sh "wifi <ssid> <password>" "key minimax <api key>" "stat"     # first time only (kept in NVS)
  ./console.sh "replay /content/test/zh_01.wav 3"                          # one call, three turns
  ./monitor.sh 30                                                          # 30 s of device logs
EOF
